/** @file_watch_displays_poll.c
 *
 *  Watch for display changes without using UDEV
 */

// Copyright (C) 2018-2024 Sanford Rockowitz <rockowitz@minsoft.com>
// SPDX-License-Identifier: GPL-2.0-or-later

#include "config.h"
#include "public/ddcutil_types.h"

/** \cond */
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <glib-2.0/glib.h>
#ifdef ENABLE_UDEV
#include <libudev.h>
#endif
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "util/coredefs.h"
#include "util/data_structures.h"
#include "util/drm_common.h"
#include "util/file_util.h"
#include "util/glib_string_util.h"
#include "util/glib_util.h"
#include "util/linux_util.h"
#include "util/report_util.h"
#include "util/string_util.h"
#include "util/sysfs_filter_functions.h"
#include "util/sysfs_i2c_util.h"
#include "util/sysfs_util.h"
#include "util/udev_util.h"

#include "base/core.h"
#include "base/displays.h"
#include "base/ddc_errno.h"
#include "base/drm_connector_state.h"
#include "base/i2c_bus_base.h"
#include "base/linux_errno.h"
#include "base/rtti.h"
#include "base/sleep.h"
/** \endcond */

#include "i2c/i2c_bus_core.h"
#include "i2c/i2c_dpms.h"
#include "i2c/i2c_sys_drm_connector.h"

#include "ddc/ddc_displays.h"
#include "ddc/ddc_watch_displays_common.h"
#include "ddc/ddc_packet_io.h"
#include "ddc/ddc_status_events.h"
#include "ddc/ddc_vcp.h"

#include "ddc/ddc_watch_displays_poll.h"



// Trace class for this file
static DDCA_Trace_Group TRACE_GROUP = DDCA_TRC_NONE;

int  nonudev_poll_loop_millisec = DEFAULT_UDEV_POLL_LOOP_MILLISEC;   // 2000;   // default sleep time on each loop

#ifdef OLD
//
//  Variant Watch_Mode_Full_Poll
//

// #ifdef WATCH_MODE_FULL_POLL

/** Primary function to check for changes in display status (disconnect, DPMS),
 *  modify internal data structures, and emit client notifications.
 */
DDCA_Display_Event_Class event_classes;
void ddc_poll_recheck_bus(
      DDCA_Display_Event_Class event_classes,
      BS256                    old_attached_buses_bitset,
      BS256                    old_buses_with_edid_bitset,
      GArray *                 deferred_events )
{
   bool debug = false;
   DBGTRC_STARTING(debug, DDCA_TRC_NONE, "");

   bool report_events = true;

   // DBGTRC_NOPREFIX(debug, DDCA_TRC_NONE, "old_connected_buses_bitset has %d bits set", bs256_count(old_buses_with_edid_bitset));
   DBGTRC_NOPREFIX(debug, DDCA_TRC_NONE, "old_attached_buses_bitset: %s", bs256_to_string_decimal_t(old_attached_buses_bitset, "", ",") );
   DBGTRC_NOPREFIX(debug, DDCA_TRC_NONE, "old_buses_with_edid_bitset: %s",bs256_to_string_decimal_t(old_buses_with_edid_bitset, "", ","));

   Bit_Set_256  cur_attached_buses_bitset = i2c_detect_attached_buses_as_bitset();
   Bit_Set_256  newly_attached_buses_bitset = bs256_and_not(cur_attached_buses_bitset, old_attached_buses_bitset);
   Bit_Set_256  newly_detached_buses_bitset = bs256_and_not(old_attached_buses_bitset, cur_attached_buses_bitset);

   bool changed = false;
   if (bs256_count(newly_detached_buses_bitset) > 0) {
      Bit_Set_256_Iterator iter = bs256_iter_new(newly_detached_buses_bitset);
      int busno;
      while(true) {
         busno = bs256_iter_next(iter);
         if (busno < 0)
            break;
#ifdef FUTURE
         DDCA_IO_Path iopath;
         iopath.io_mode = DDCA_IO_I2C;
         iopath.path.i2c_busno = busno;
         // connector events not currently being reported
         // ddc_emit_display_detection_event(DDCA_EVENT_CONNECTOR_REMOVED, NULL, NULL, iopath);
#endif
      }
      bs256_iter_free(iter);
      changed = true;
   }

   if (bs256_count(newly_attached_buses_bitset) > 0) {
      Bit_Set_256_Iterator iter = bs256_iter_new(newly_attached_buses_bitset);
      int busno;
      while(true) {
         busno = bs256_iter_next(iter);
         if (busno < 0)
            break;
         DDCA_IO_Path iopath;
         iopath.io_mode = DDCA_IO_I2C;
         iopath.path.i2c_busno = busno;

         I2C_Bus_Info * new_businfo = i2c_new_bus_info(iopath.path.i2c_busno);
         new_businfo->flags = I2C_BUS_VALID_NAME_CHECKED | I2C_BUS_HAS_VALID_NAME | I2C_BUS_EXISTS;
         i2c_check_bus2(new_businfo);
         g_ptr_array_add(all_i2c_buses, new_businfo);
         DBGTRC_NOPREFIX(debug || report_events, DDCA_TRC_NONE, "Added businfo for bus /dev/i2c-%d", new_businfo->busno);
         if (IS_DBGTRC(debug, DDCA_TRC_NONE))
            i2c_dbgrpt_bus_info(new_businfo, /*include_sysinfo*/ true, 1);
         DBGTRC_NOPREFIX(debug|| report_events, DDCA_TRC_NONE, "Emitting DDCA_EVENT_BUS_ATTACHED for bus /dev/i2c-%d", iopath.path.i2c_busno);
         // connector events not currently being reported
         // ddc_emit_display_detection_event(DDCA_EVENT_CONNECTOR_ADDED, new_businfo->drm_connector_name, NULL, iopath);
      }
      bs256_iter_free(iter);
      changed = true;
   }

   if (changed) {
      DBGMSG("changed == true");
      // get_sys_drm_connectors(/*rescan=*/true);
   }

   GPtrArray * new_buses = i2c_detect_buses0();
   Bit_Set_256 new_buses_with_edid_bitset = buses_bitset_from_businfo_array(new_buses, true);
   // DBGTRC_NOPREFIX(debug, DDCA_TRC_NONE, "new_bitset has %d bits set", bs256_count(new_bitset));

   Bit_Set_256 newly_disconnected_buses_bitset = bs256_and_not(old_buses_with_edid_bitset, new_buses_with_edid_bitset);
   Bit_Set_256 newly_connected_buses_bitset    = bs256_and_not(new_buses_with_edid_bitset, old_buses_with_edid_bitset);
   int ct =  bs256_count(newly_disconnected_buses_bitset);
   if (ct > 0)
      DBGTRC_NOPREFIX(debug|| report_events, DDCA_TRC_NONE, "newly_disconnected_buses_bitset has %d bits set", ct);
   ct = bs256_count(newly_connected_buses_bitset);
   if (ct > 0)
      DBGTRC_NOPREFIX(debug|| report_events, DDCA_TRC_NONE, "newly_connected_buses_bitset has %d bits set", ct);

   Bit_Set_256_Iterator iter = bs256_iter_new(newly_disconnected_buses_bitset);
   int busno;
   while(true) {
      busno = bs256_iter_next(iter);
      if (busno < 0)
         break;
      I2C_Bus_Info * businfo =  i2c_find_bus_info_in_gptrarray_by_busno(all_i2c_buses, busno);
      Display_Ref * dref = ddc_remove_display_by_businfo(businfo);
      ddc_emit_or_queue_display_status_event(DDCA_EVENT_DISPLAY_DISCONNECTED,
                                      businfo->drm_connector_name,
                                      dref, dref->io_path,
                                      NULL);
      if (i2c_device_exists(businfo->busno)) {
         i2c_reset_bus_info(businfo);
      }
      else {
         g_ptr_array_remove(all_i2c_buses, businfo);
      }

      // get_sys_drm_connectors(/*rescan=*/true);
   }
   bs256_iter_free(iter);

   iter = bs256_iter_new(newly_connected_buses_bitset);
   while(true) {
      busno = bs256_iter_next(iter);
      if (busno < 0)
         break;

      DBGTRC_NOPREFIX(debug, DDCA_TRC_NONE, "Newly detected busno %d", busno);
      int new_index = i2c_find_bus_info_index_in_gptrarray_by_busno(new_buses, busno);
      I2C_Bus_Info * new_businfo = g_ptr_array_index(new_buses, new_index);
      if (IS_DBGTRC(debug, DDCA_TRC_NONE)) {
         DBGTRC_NOPREFIX(debug, DDCA_TRC_NONE, "new_businfo: /dev/i2c-%d @%p",
               new_businfo->busno, new_businfo);
         // i2c_dbgrpt_bus_info(new_businfo, 4);
      }
      I2C_Bus_Info * old_businfo = i2c_find_bus_info_in_gptrarray_by_busno(all_i2c_buses, busno);
      if (old_businfo) {
         DBGTRC_NOPREFIX(debug, DDCA_TRC_NONE, "Updating businfo for /dev/i2c-%d", old_businfo->busno);
         i2c_update_bus_info(old_businfo, new_businfo);
         Display_Ref * dref = ddc_add_display_by_businfo(old_businfo);
         ddc_emit_or_queue_display_status_event(DDCA_EVENT_DISPLAY_CONNECTED,
                             old_businfo->drm_connector_name, dref, dref->io_path, NULL);
      }
      else {
         DBGTRC_NOPREFIX(debug|| report_events, DDCA_TRC_NONE,
               "Adding businfo for newly detected /dev/i2c-%d", new_businfo->busno);
         // g_ptr_array_steal_index() requires glib 2.58
         // g_ptr_array_steal_index(new_buses, new_index);
         // should not be set, but just in case:
         g_ptr_array_set_free_func(new_buses, NULL);
         g_ptr_array_remove_index(new_buses, new_index);
         g_ptr_array_add(all_i2c_buses, new_businfo);
         Display_Ref * dref = ddc_add_display_by_businfo(new_businfo);
         ddc_emit_or_queue_display_status_event(DDCA_EVENT_DISPLAY_CONNECTED,
                             new_businfo->drm_connector_name, dref, dref->io_path, NULL);

      }
   }
   bs256_iter_free(iter);
   g_ptr_array_set_free_func(new_buses, (GDestroyNotify) i2c_free_bus_info);
   g_ptr_array_free(new_buses, true);

   // DBGTRC_NOPREFIX(debug, DDCA_TRC_NONE, "Checking sleep state");
   assert(all_i2c_buses);
   for (int ndx = 0; ndx < all_i2c_buses->len; ndx++) {
      I2C_Bus_Info * businfo = g_ptr_array_index(all_i2c_buses, ndx);
      // DBGTRC_NOPREFIX(debug, DDCA_TRC_NONE, "ndx=%d, businfo=%p", ndx, businfo);
      // DBGTRC_NOPREFIX(debug, DDCA_TRC_NONE, "bus=%d", businfo->busno);
      if (businfo->flags & I2C_BUS_ADDR_0X50) {
         // DBGTRC_NOPREFIX(debug, DDCA_TRC_NONE, "bus=%d, I2C_BUS_ADDR_0X50 set", businfo->busno);
         bool is_dpms_asleep = dpms_check_drm_asleep_by_businfo(businfo);
         DBGTRC_NOPREFIX(debug, DDCA_TRC_NONE, "busno=%d, is_dpms_asleep=%s, last_checked_dpms_asleep=%s",
               businfo->busno, sbool(is_dpms_asleep), sbool(businfo->last_checked_dpms_asleep));
         if (is_dpms_asleep != businfo->last_checked_dpms_asleep) {
            Display_Ref * dref = ddc_get_dref_by_busno_or_connector(businfo->busno, NULL, /*ignore_invalid*/ true);
            assert(dref);
            DBGTRC_NOPREFIX(debug, DDCA_TRC_NONE, "sleep change event for dref=%p->%s", dref, dref_repr_t(dref));
            DDCA_Display_Event_Type event_type = (is_dpms_asleep) ? DDCA_EVENT_DPMS_ASLEEP : DDCA_EVENT_DPMS_AWAKE;
            ddc_emit_or_queue_display_status_event(event_type, dref->drm_connector, dref, dref->io_path, NULL);
            businfo->last_checked_dpms_asleep = is_dpms_asleep;
         }
      }
   }

   DBGTRC_DONE(debug, DDCA_TRC_NONE, "");
}

#endif


gpointer ddc_watch_displays_without_udev(gpointer data) {
   bool debug = false;
   // bool debug_sysfs_state = false;
   bool use_deferred_event_queue = false;
   // bool report_events = true;
   Watch_Displays_Data * wdd = data;
   assert(wdd && memcmp(wdd->marker, WATCH_DISPLAYS_DATA_MARKER, 4) == 0);

   DBGTRC_STARTING(debug, DDCA_TRC_NONE,
         "Caller process id: %d, caller thread id: %d, event_classes=0x%02x",
         wdd->main_process_id, wdd->main_thread_id, wdd->event_classes);
   DBGTRC_NOPREFIX(debug, DDCA_TRC_NONE, "Watching for display connection events: %s",
         sbool(wdd->event_classes & DDCA_EVENT_CLASS_DISPLAY_CONNECTION));
   DBGTRC_NOPREFIX(debug, DDCA_TRC_NONE, "Watching for dpms events: %s",
          sbool(wdd->event_classes & DDCA_EVENT_CLASS_DPMS));

   // may need to wait on startup
   while (!all_i2c_buses) {
      DBGMSF(debug, "Waiting 1 sec for all_i2c_buses");
      // usleep(1000*1000);
      sleep_millis(1000*1000);
   }

   bool watch_dpms = wdd->event_classes & DDCA_EVENT_CLASS_DPMS;

   pid_t cur_pid = getpid();
   pid_t cur_tid = get_thread_id();
   DBGTRC_NOPREFIX(debug, DDCA_TRC_NONE, "Our process id: %d, our thread id: %d", cur_pid, cur_tid);

   BS256 bs_sleepy_buses   = EMPTY_BIT_SET_256;
   BS256 bs_old_attached_buses = buses_bitset_from_businfo_array(all_i2c_buses, false);
   BS256 bs_old_buses_w_edid   = buses_bitset_from_businfo_array(all_i2c_buses, true);

   DBGTRC_NOPREFIX(debug, TRACE_GROUP, "Initial i2c buses with edids: %s",
          BS256_REPR(bs_old_buses_w_edid));
   if (IS_DBGTRC(debug, DDCA_TRC_NONE) && false) {
       rpt_vstring(0, "Initial I2C buses:");
       i2c_dbgrpt_buses_summary(1);
       rpt_vstring(0, "Initial Display Refs:");
       ddc_dbgrpt_display_refs_summary(true,     // include_invalid_displays
                                       false,    // report_businfo
                                       1);       // depth
       if (use_drm_connector_states) {
          rpt_vstring(0, "Initial DRM connector states");
          report_drm_connector_states_basic(/*refresh*/ true, 1);
       }
    }

   GArray * deferred_events = NULL;
   if (use_deferred_event_queue)
      deferred_events = g_array_new( false,      // zero_terminated
                                     false,       // clear
                                     sizeof(DDCA_Display_Status_Event));
   bool skip_next_sleep = false;
   int slept = 0;   // will contain length of final sleep
   while (!terminate_watch_thread) {
      if (deferred_events && deferred_events->len > 0) {
         ddc_i2c_emit_deferred_events(deferred_events);
      }
      else {     // skip polling loop sleep if deferred events were output
         if (!skip_next_sleep) {
            slept = split_sleep();
         }
      }
      skip_next_sleep = false;
      if (terminate_watch_thread)
         continue;
      terminate_if_invalid_thread_or_process(cur_pid, cur_tid);

#ifdef OLD
      GPtrArray * cur_buses = i2c_detect_buses0();
      Bit_Set_256 bs_cur_buses_w_edid   = buses_bitset_from_businfo_array(cur_buses, true);
      Bit_Set_256 bs_cur_attached_buses = buses_bitset_from_businfo_array(cur_buses, false);
#endif

      Bit_Set_256 bs_cur_attached_buses = i2c_detect_attached_buses_as_bitset();
      Bit_Set_256 bs_cur_buses_w_edid   = i2c_filter_buses_w_edid_as_bitset(bs_cur_attached_buses);

      Bit_Set_256 bs_added_buses_w_edid     = bs256_and_not(bs_cur_buses_w_edid, bs_old_buses_w_edid);
      Bit_Set_256 bs_removed_buses_w_edid   = bs256_and_not(bs_old_buses_w_edid, bs_cur_buses_w_edid);
      Bit_Set_256 bs_added_attached_buses   = bs256_and_not(bs_cur_attached_buses, bs_old_attached_buses);
      Bit_Set_256 bs_removed_attached_buses = bs256_and_not(bs_old_attached_buses, bs_cur_attached_buses);
#ifdef TMI
      DBGTRC_NOPREFIX(debug, DDCA_TRC_NONE, "bs_old_buses_w_edid(0): %s",
                                bs256_to_string_decimal_t(bs_old_buses_w_edid, "", ","));
      DBGTRC_NOPREFIX(debug, DDCA_TRC_NONE, "bs_cur_buses_w_edid(0): %s",
                                bs256_to_string_decimal_t(bs_cur_buses_w_edid, "", ","));
#endif

      if ( bs256_count(bs_removed_buses_w_edid) > 0 ) {   // || bs256_count(bs_removed_attached_buses) > 0) {
         DBGTRC_NOPREFIX(debug, DDCA_TRC_NONE, "bs_old_attached_buses: %s", BS256_REPR(bs_old_attached_buses));
         DBGTRC_NOPREFIX(debug, DDCA_TRC_NONE, "bs_cur_attached_buses: %s", BS256_REPR(bs_cur_attached_buses));
         DBGTRC_NOPREFIX(debug, DDCA_TRC_NONE, "bs_old_buses_w_edid: %s",   BS256_REPR(bs_old_buses_w_edid));
         DBGTRC_NOPREFIX(debug, DDCA_TRC_NONE, "bs_cur_buses_w_edid: %s",   BS256_REPR(bs_cur_buses_w_edid));

#ifdef OLD
         cur_buses = ddc_i2c_stabilized_buses(cur_buses, true);
         bs_cur_buses_w_edid   = buses_bitset_from_businfo_array(cur_buses, true);
         bs_cur_attached_buses = buses_bitset_from_businfo_array(cur_buses, false);
#endif

         bs_cur_buses_w_edid = ddc_i2c_stabilized_buses_bs(bs_cur_buses_w_edid, bs256_count(bs_removed_buses_w_edid));

         BS256 bs_added_buses_w_edid     = bs256_and_not(bs_cur_buses_w_edid, bs_old_buses_w_edid);
         bs_removed_buses_w_edid   = bs256_and_not(bs_old_buses_w_edid, bs_cur_buses_w_edid);
         bs_added_attached_buses   = bs256_and_not(bs_cur_attached_buses, bs_old_attached_buses);
         bs_removed_attached_buses = bs256_and_not(bs_old_attached_buses, bs_cur_attached_buses);
         DBGTRC_NOPREFIX(debug, DDCA_TRC_NONE, "After stabilization:");
         DBGTRC_NOPREFIX(debug, DDCA_TRC_NONE, "bs_old_attached_buses: %s", BS256_REPR(bs_old_attached_buses));
         DBGTRC_NOPREFIX(debug, DDCA_TRC_NONE, "bs_cur_attached_buses: %s", BS256_REPR(bs_cur_attached_buses));
         DBGTRC_NOPREFIX(debug, DDCA_TRC_NONE, "bs_old_buses_w_edid:   %s", BS256_REPR(bs_old_buses_w_edid));
         DBGTRC_NOPREFIX(debug, DDCA_TRC_NONE, "bs_cur_buses_w_edid:   %s", BS256_REPR(bs_cur_buses_w_edid));
         DBGTRC_NOPREFIX(debug, DDCA_TRC_NONE, "bs_added_attached_buses:   %s", BS256_REPR(bs_added_attached_buses));
         DBGTRC_NOPREFIX(debug, DDCA_TRC_NONE, "bs_removed_attached_buses:   %s", BS256_REPR(bs_removed_attached_buses));
         DBGTRC_NOPREFIX(debug, DDCA_TRC_NONE, "bs_added_buses_w_edid: %s", BS256_REPR(bs_added_buses_w_edid));
         DBGTRC_NOPREFIX(debug, DDCA_TRC_NONE, "bs_removed_buses_w_edid: %s", BS256_REPR(bs_removed_buses_w_edid));

      }
#ifdef OLD
      g_ptr_array_free(cur_buses, true);
#endif
      bs_old_buses_w_edid   = bs_cur_buses_w_edid;
      bs_old_attached_buses = bs_cur_attached_buses;

      bool hotplug_change_handler_emitted = false;
      // bool connected_buses_changed = !bs256_eq( bs_prev_buses_w_edid, bs_new_buses_w_edid);
      bool connected_buses_w_edid_changed = bs256_count(bs_removed_buses_w_edid) > 0 ||
                                          bs256_count(bs_added_buses_w_edid) > 0;

      DBGTRC_NOPREFIX(debug, DDCA_TRC_NONE, "connected_buses_changed = %s", SBOOL(connected_buses_w_edid_changed));

      if (connected_buses_w_edid_changed) {
         // BS256 bs_buses_w_edid_removed = bs256_and_not(bs_prev_buses_w_edid, bs_new_buses_w_edid);
         // DBGTRC_NOPREFIX(debug, TRACE_GROUP, "bs_buses_w_edid_removed: %s", BS256_REPR(bs_buses_w_edid_removed));

         // BS256 bs_buses_w_edid_added = bs256_and_not(bs_new_buses_w_edid, bs_prev_buses_w_edid);
         // DBGTRC_NOPREFIX(debug, TRACE_GROUP, "bs_buses_w_edid_added: %s", BS256_REPR(bs_buses_w_edid_added));

         hotplug_change_handler_emitted = ddc_i2c_hotplug_change_handler(
                                              bs_removed_buses_w_edid,
                                              bs_added_buses_w_edid,
                                              deferred_events);
      }

      if (hotplug_change_handler_emitted)
         DBGTRC_NOPREFIX(debug, DDCA_TRC_NONE, "hotplug_change_handler_emitted = %s",
               sbool (hotplug_change_handler_emitted));



#ifdef OUT
      if ( bs256_count(bs_removed_buses_w_edid) > 0) {
         Bit_Set_256_Iterator iter = bs256_iter_new(bs_removed_buses_w_edid);
         int busno;
         while(true) {
            busno = bs256_iter_next(iter);
            if (busno < 0)
               break;

            DBGTRC_NOPREFIX(debug || true, DDCA_TRC_NONE, "Disconnected display on bus %d", busno);
            I2C_Bus_Info * businfo =  i2c_find_bus_info_in_gptrarray_by_busno(all_i2c_buses, busno);
            Display_Ref * dref = ddc_remove_display_by_businfo(businfo);
            ddc_emit_or_queue_display_status_event(DDCA_EVENT_DISPLAY_DISCONNECTED,
                                            businfo->drm_connector_name,
                                            dref, dref->io_path,
                                            NULL);
         }
         bs256_iter_free(iter);
      }

      if ( bs256_count(bs_removed_attached_buses) > 0) {
      Bit_Set_256_Iterator iter = bs256_iter_new(bs_removed_attached_buses);
         int busno;
         while(true) {
            busno = bs256_iter_next(iter);
            if (busno < 0)
               break;

            DBGTRC_NOPREFIX(debug || true, DDCA_TRC_NONE, "Detached bus %d", busno);
            I2C_Bus_Info * businfo =  i2c_find_bus_info_in_gptrarray_by_busno(all_i2c_buses, busno);
            i2c_remove_bus_info(busno);
            i2c_free_bus_info(businfo);
         }
         bs256_iter_free(iter);
      }

       if ( bs256_count(bs_added_attached_buses) > 0) {
          Bit_Set_256_Iterator iter = bs256_iter_new(bs_added_attached_buses);
          int busno;
          while(true) {
             busno = bs256_iter_next(iter);
             if (busno < 0)
                break;

             DBGTRC_NOPREFIX(debug || true, DDCA_TRC_NONE, "Attach new bus %d", busno);
             I2C_Bus_Info * businfo = i2c_new_bus_info(busno);
             businfo->flags &= ~I2C_BUS_PROBED;
             i2c_check_bus2(businfo);
             i2c_add_bus_info(businfo);
          }
          bs256_iter_free(iter);
      }

      // DBGTRC_NOPREFIX(debug, DDCA_TRC_NONE, "bs_added_buses_w_edid(2): %s", BS256_REPR(bs_added_buses_w_edid));
      if ( bs256_count(bs_added_buses_w_edid) > 0) {
         Bit_Set_256_Iterator iter = bs256_iter_new(bs_added_buses_w_edid);
         while (true) {
            int busno = bs256_iter_next(iter);
            if (busno < 0)
               break;

            DBGTRC_NOPREFIX(debug || true, DDCA_TRC_NONE, "Connected display on bus %d", busno);
            I2C_Bus_Info * businfo =  i2c_find_bus_info_in_gptrarray_by_busno(all_i2c_buses, busno);
            assert(businfo);
            Display_Ref * dref = ddc_add_display_by_businfo(businfo);
            ddc_emit_or_queue_display_status_event(DDCA_EVENT_DISPLAY_CONNECTED,
                             businfo->drm_connector_name, dref, dref->io_path, NULL);
         }
      }
#endif

      if (watch_dpms) {
         // DBGTRC_NOPREFIX(debug, TRACE_GROUP, "Before ddc_check_bus_asleep(), bs_sleepy_buses: %s",
         //      BS256_REPR(bs_sleepy_buses));
         // emits dpms events directly or places them on deferred_events queue
         bs_sleepy_buses = ddc_i2c_check_bus_asleep(
               bs_old_buses_w_edid, bs_sleepy_buses, deferred_events);
         // DBGTRC_NOPREFIX(debug, TRACE_GROUP, "After ddc_check_bus_asleep(), bs_sleepy_buses: %s",
         //       BS256_REPR(bs_sleepy_buses));
      }

#ifdef ALT
      if (watch_dpms) {
         for (int ndx = 0; ndx < all_i2c_buses->len; ndx++) {
             I2C_Bus_Info * businfo = g_ptr_array_index(all_i2c_buses, ndx);
             // DBGTRC_NOPREFIX(debug, DDCA_TRC_NONE, "ndx=%d, businfo=%p", ndx, businfo);
             // DBGTRC_NOPREFIX(debug, DDCA_TRC_NONE, "bus=%d", businfo->busno);
             if (businfo->flags & I2C_BUS_ADDR_0X50) {
                 // DBGTRC_NOPREFIX(debug, DDCA_TRC_NONE, "bus=%d, I2C_BUS_ADDR_0X50 set", businfo->busno);
                 bool is_dpms_asleep = dpms_check_drm_asleep_by_businfo(businfo);
                 DBGTRC_NOPREFIX(debug, DDCA_TRC_NONE, "busno=%d, is_dpms_asleep=%s, last_checked_dpms_asleep=%s",
                    businfo->busno, sbool(is_dpms_asleep), sbool(businfo->last_checked_dpms_asleep));
                 if (is_dpms_asleep != businfo->last_checked_dpms_asleep) {
                    Display_Ref * dref = ddc_get_dref_by_busno_or_connector(businfo->busno, NULL, /*ignore_invalid*/ true);
                    assert(dref);
                    DBGTRC_NOPREFIX(debug, DDCA_TRC_NONE, "sleep change event for dref=%p->%s", dref, dref_repr_t(dref));
                    DDCA_Display_Event_Type event_type = (is_dpms_asleep) ? DDCA_EVENT_DPMS_ASLEEP : DDCA_EVENT_DPMS_AWAKE;
                    ddc_emit_or_queue_display_status_event(event_type, dref->drm_connector, dref, dref->io_path, NULL);
                    businfo->last_checked_dpms_asleep = is_dpms_asleep;
                 }
              }
          }
      }
#endif

   } // while()

   // n. slept == 0 if no sleep was performed
   DBGTRC_DONE(debug, TRACE_GROUP,
         "Terminating thread.  Final polling sleep was %d millisec.", slept/1000);
   free_watch_displays_data(wdd);
   if (deferred_events)
      g_array_free(deferred_events, true);
#ifdef WATCH_DPMS
   if (watch_dpms)
      g_ptr_array_free(sleepy_connectors, true);
#endif
   g_thread_exit(0);
   assert(false);    // avoid clang warning re wdd use after free
   return NULL;    // satisfy compiler check that value returned
}


void init_ddc_watch_displays_poll() {
      RTTI_ADD_FUNC(ddc_watch_displays_without_udev);
}
