/*
 * Soft:        Keepalived is a failover program for the LVS project
 *              <www.linuxvirtualserver.org>. It monitor & manipulate
 *              a loadbalanced server pool using multi-layer checks.
 *
 * Part:        VRRP synchronization framework.
 *
 * Author:      Alexandre Cassen, <acassen@linux-vs.org>
 *
 *              This program is distributed in the hope that it will be useful,
 *              but WITHOUT ANY WARRANTY; without even the implied warranty of
 *              MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 *              See the GNU General Public License for more details.
 *
 *              This program is free software; you can redistribute it and/or
 *              modify it under the terms of the GNU General Public License
 *              as published by the Free Software Foundation; either version
 *              2 of the License, or (at your option) any later version.
 *
 * Copyright (C) 2001-2017 Alexandre Cassen, <acassen@gmail.com>
 */

#include "config.h"

#include <stdbool.h>

#include "vrrp_sync.h"
#include "vrrp_track.h"
#include "vrrp_notify.h"
#include "vrrp_data.h"
#include "logger.h"
#include "vrrp_scheduler.h"
#include "parser.h"

/* Instance name lookup */
vrrp_t * __attribute__ ((pure))
vrrp_get_instance(char *iname)
{
	vrrp_t *vrrp;

	list_for_each_entry(vrrp, &vrrp_data->vrrp, e_list) {
		if (strcmp(vrrp->iname, iname) == 0)
			return vrrp;
	}
	return NULL;
}

/* Set instances group pointer */
bool
vrrp_sync_set_group(vrrp_sgroup_t *sgroup)
{
	vrrp_t *vrrp;
	char *str;
	unsigned int i;
	bool group_member_down = false;

	/* Can't handle no members of the group */
	if (!sgroup->iname)
		return false;

	for (i = 0; i < vector_size(sgroup->iname); i++) {
		str = vector_slot(sgroup->iname, i);
		vrrp = vrrp_get_instance(str);
		if (!vrrp) {
			report_config_error(CONFIG_GENERAL_ERROR, "Virtual router %s specified in sync group %s doesn't exist - ignoring", str, sgroup->gname);
			continue;
		}

		if (vrrp->sync) {
			report_config_error(CONFIG_GENERAL_ERROR, "Virtual router %s cannot exist in more than one sync group; ignoring %s", str, sgroup->gname);
			continue;
		}

		list_add_tail(&vrrp->s_list, &sgroup->vrrp_instances);
		vrrp->sync = sgroup;

		/* set eventual sync group state. Unless all members are master and address owner,
		 * then we must be backup */
		if (sgroup->state == VRRP_STATE_MAST && vrrp->wantstate == VRRP_STATE_BACK)
			report_config_error(CONFIG_GENERAL_ERROR, "Sync group %s has some member(s) as address owner and some not as address owner. This won't work.", sgroup->gname);
		if (sgroup->state != VRRP_STATE_BACK)
			sgroup->state = (vrrp->wantstate == VRRP_STATE_MAST && vrrp->base_priority == VRRP_PRIO_OWNER) ? VRRP_STATE_MAST : VRRP_STATE_BACK;

// TODO - what about track scripts down?
		if (vrrp->state == VRRP_STATE_FAULT)
			group_member_down = true;
	}

	/* The iname vector is only used for us to set up the sync groups, so delete it */
	free_strvec(sgroup->iname);
	sgroup->iname = NULL;

	if (group_member_down)
		sgroup->state = VRRP_STATE_FAULT;

	/* The sync group will be removed by the calling function if it has no members */
	if (list_empty(&sgroup->vrrp_instances)) {
		report_config_error(CONFIG_GENERAL_ERROR, "Sync group %s, no matching virtual router found"
							  " in group declaration - removing"
							, sgroup->gname);
		return false;
	}

	/* For most users a sync group with only one member is a configuration error */
	if (sgroup->vrrp_instances.prev == sgroup->vrrp_instances.next)
		report_config_error(CONFIG_GENERAL_ERROR, "Sync group %s has only 1 virtual router(s) -"
							  " this probably isn't what you want"
							, sgroup->gname);

	return true;
}

/* Check transition to master state */
bool
vrrp_sync_can_goto_master(vrrp_t *vrrp)
{
	vrrp_sgroup_t *sgroup = vrrp->sync;
	vrrp_t *isync;

	if (GROUP_STATE(sgroup) == VRRP_STATE_MAST)
		return true;

	/* Only sync to master if everyone wants to
	 * i.e. prefer backup state to avoid thrashing */
	list_for_each_entry(isync, &sgroup->vrrp_instances, s_list) {
		if (isync != vrrp && isync->wantstate != VRRP_STATE_MAST) {
			/* Make sure we give time for other instances to be
			 * ready to become master. The timer here doesn't
			 * really matter, since we are waiting for other
			 * instances to be ready. */
			vrrp->ms_down_timer = VRRP_MS_DOWN_TIMER(vrrp);
			vrrp_init_instance_sands(vrrp);
			return false;
		}
	}

	return true;
}

void
vrrp_sync_backup(vrrp_t *vrrp)
{
	vrrp_sgroup_t *sgroup = vrrp->sync;
	vrrp_t *isync;

	if (GROUP_STATE(sgroup) == VRRP_STATE_BACK)
		return;

	log_message(LOG_INFO, "VRRP_Group(%s) Syncing instances to BACKUP state"
			    , GROUP_NAME(sgroup));

	/* Perform sync index */
	list_for_each_entry(isync, &sgroup->vrrp_instances, s_list) {
		if (isync == vrrp || isync->state == VRRP_STATE_BACK)
			continue;

		isync->wantstate = VRRP_STATE_BACK;
// TODO - we may be leaving FAULT, so calling leave_master isn't right. I have
// had to add vrrp_state_leave_fault() for this
		if (isync->state == VRRP_STATE_FAULT ||
		    isync->state == VRRP_STATE_INIT) {
			vrrp_state_leave_fault(isync);
		}
		else
			vrrp_state_leave_master(isync, false);
		vrrp_thread_requeue_read(isync);
	}

	sgroup->state = VRRP_STATE_BACK;
	send_group_notifies(sgroup);
}

void
vrrp_sync_master(vrrp_t *vrrp)
{
	vrrp_sgroup_t *sgroup = vrrp->sync;
	vrrp_t *isync;

	if (GROUP_STATE(sgroup) == VRRP_STATE_MAST)
		return;
	if (!vrrp_sync_can_goto_master(vrrp))
		return;

	log_message(LOG_INFO, "VRRP_Group(%s) Syncing instances to MASTER state"
			    , GROUP_NAME(sgroup));

	/* Perform sync index */
	list_for_each_entry(isync, &sgroup->vrrp_instances, s_list) {

// TODO		/* Send the higher priority advert on all synced instances */
		if (isync != vrrp && isync->state != VRRP_STATE_MAST) {
			isync->wantstate = VRRP_STATE_MAST;
// TODO 6 - transition straight to master if PRIO_OWNER
// TODO 7 - not here, but generally if wantstate == MAST && !owner, ms_down_timer = adver_int + 1 skew and be backup
//			if (vrrp->wantstate == VRRP_STATE_MAST && vrrp->base_priority == VRRP_PRIO_OWNER) {
//				/* ??? */
//			} else {
#ifdef _WITH_SNMP_RFCV3_
				isync->stats->next_master_reason = vrrp->stats->master_reason;
#endif
				vrrp_state_goto_master(isync);
				vrrp_thread_requeue_read(isync);
//			}
		}
	}

	sgroup->state = VRRP_STATE_MAST;
	send_group_notifies(sgroup);
}

void
vrrp_sync_fault(vrrp_t *vrrp)
{
	vrrp_sgroup_t *sgroup = vrrp->sync;
	vrrp_t *isync;

	if (GROUP_STATE(sgroup) == VRRP_STATE_FAULT)
		return;

	log_message(LOG_INFO, "VRRP_Group(%s) Syncing instances to FAULT state"
			    , GROUP_NAME(sgroup));

	/* Perform sync index */
	list_for_each_entry(isync, &sgroup->vrrp_instances, s_list) {
		/* We force sync instance to backup mode.
		 * This reduce instance takeover to less than ms_down_timer.
		 * => by default ms_down_timer is set to 3secs.
		 * => Takeover will be less than 3secs !
		 */
		if (isync != vrrp && isync->state != VRRP_STATE_FAULT) {
			isync->wantstate = VRRP_STATE_FAULT;
			if (isync->state == VRRP_STATE_MAST) {
				vrrp_state_leave_master(isync, false);
			}
			else if (isync->state == VRRP_STATE_BACK || isync->state == VRRP_STATE_INIT) {
				isync->state = VRRP_STATE_FAULT;	/* This is a bit of a bodge */
				vrrp_state_leave_fault(isync);
			}
		}
	}

	sgroup->state = VRRP_STATE_FAULT;
	send_group_notifies(sgroup);
}
