/* 
 * This file is part of the Mooltipass Project (https://github.com/mooltipass).
 * Copyright (c) 2019 Stephan Mathieu
 * 
 * This program is free software: you can redistribute it and/or modify  
 * it under the terms of the GNU General Public License as published by  
 * the Free Software Foundation, version 3.
 *
 * This program is distributed in the hope that it will be useful, but 
 * WITHOUT ANY WARRANTY; without even the implied warranty of 
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU 
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License 
 * along with this program. If not, see <http://www.gnu.org/licenses/>.
 */
/*!  \file     logic_device.c
*    \brief    General logic for device
*    Created:  27/01/2019
*    Author:   Mathieu Stephan
*/
#include "gui_dispatcher.h"
#include "logic_device.h"
#include "driver_timer.h"
#include "platform_io.h"
#include "logic_power.h"
#include "custom_fs.h"
#include "sh1122.h"
#include "utils.h"
#include "main.h"
/* Platform wakeup reason */
volatile platform_wakeup_reason_te logic_device_wakeup_reason = WAKEUP_REASON_OTHER;
/* USB timeout detected bool */
BOOL logic_device_usb_timeout_detected = FALSE;
/* Device state changed bool */
BOOL logic_device_state_changed = FALSE;
/* Settings changed bool */
BOOL logic_device_settings_changed = FALSE;


/*! \fn     logic_device_set_settings_changed(void)
*   \brief  Inform that the settings have changed
*/
void logic_device_set_settings_changed(void)
{
    logic_device_settings_changed = TRUE;
}

/*! \fn     logic_device_get_and_clear_settings_changed_flag(void)
*   \brief  Get and clear settings changed flag
*   \return The flag
*/
BOOL logic_device_get_and_clear_settings_changed_flag(void)
{
    BOOL return_val = logic_device_settings_changed;
    logic_device_settings_changed = FALSE;
    return return_val;
}

/*! \fn     logic_device_get_and_clear_usb_timeout_detected(void)
*   \brief  Get and clear USB timeout detected flag
*   \return The flag
*/
BOOL logic_device_get_and_clear_usb_timeout_detected(void)
{
    BOOL ret_val = logic_device_usb_timeout_detected;
    logic_device_usb_timeout_detected = FALSE;
    return ret_val;
}

/*! \fn     logic_device_set_usb_timeout_detected(void)
*   \brief  Set USB timeout detected flag
*/
void logic_device_set_usb_timeout_detected(void)
{
    logic_device_usb_timeout_detected = TRUE;
}

/*! \fn     logic_device_set_wakeup_reason(platform_wakeup_reason_te reason)
*   \brief  Set device wakeup reason
*   \param  reason  wakeup reason
*/
void logic_device_set_wakeup_reason(platform_wakeup_reason_te reason)
{
    if (logic_device_wakeup_reason == WAKEUP_REASON_NONE)
    {
        logic_device_wakeup_reason = reason;
    }
}

/*! \fn     logic_device_clear_wakeup_reason(void)
*   \brief  Clear current wakeup reason
*/
void logic_device_clear_wakeup_reason(void)
{
    logic_device_wakeup_reason = WAKEUP_REASON_NONE;
}

/*! \fn     logic_device_get_wakeup_reason(void)
*   \brief  Get device wakeup reason
*   \return wakeup reason
*/
platform_wakeup_reason_te logic_device_get_wakeup_reason(void)
{
    return logic_device_wakeup_reason;
}

/*! \fn     logic_device_power_off(void)
*   \brief  Switch off device
*/
void logic_device_power_off(void)
{
    logic_power_power_down_actions();           // Power down actions
    sh1122_oled_off(&plat_oled_descriptor);     // Display off command
    platform_io_power_down_oled();              // Switch off stepup
    platform_io_set_wheel_click_pull_down();    // Pull down on wheel click to slowly discharge capacitor
    timer_delay_ms(100);                        // From OLED datasheet wait before removing 3V3
    platform_io_set_wheel_click_low();          // Completely discharge cap
    timer_delay_ms(10);                         // Wait a tad
    platform_io_disable_switch_and_die();       // Die!    
}

/*! \fn     logic_device_activity_detected(void)
*   \brief  Function called whenever some activity is detected
*/
void logic_device_activity_detected(void)
{
    /* Reset timers */
    #ifndef EMULATOR_BUILD
    if (platform_io_is_usb_3v3_present() == FALSE)
    {
        timer_start_timer(TIMER_SCREEN, SCREEN_TIMEOUT_MS_BAT_PWRD);
    }
    else
    {
        timer_start_timer(TIMER_SCREEN, SCREEN_TIMEOUT_MS);        
    }    
    timer_start_timer(TIMER_USER_INTERACTION, utils_check_value_for_range(custom_fs_settings_get_device_setting(SETTING_USER_INTERACTION_TIMEOUT_ID), SETTING_MIN_USER_INTERACTION_TIMEOUT, SETTING_MAX_USER_INTERACTION_TIMOUT) << 10);
    #else
    timer_start_timer(TIMER_SCREEN, EMULATOR_SCREEN_TIMEOUT_MS);
    timer_start_timer(TIMER_USER_INTERACTION, SETTING_MAX_USER_INTERACTION_TIMOUT_EMU << 10);
    #endif
    
    /* Re-arm logoff timer if feature is enabled */
    uint16_t nb_20mins_ticks_before_lock_setting = custom_fs_settings_get_device_setting(SETTINGS_NB_20MINS_TICKS_FOR_LOCK);
    if (nb_20mins_ticks_before_lock_setting != 0)
    {
        timer_start_logoff_timer(nb_20mins_ticks_before_lock_setting);
    }
    
    /* Check for screen off, switch it on if so */
    if (sh1122_is_oled_on(&plat_oled_descriptor) == FALSE)
    {
        if (platform_io_is_usb_3v3_present() == FALSE)
        {
            logic_power_set_power_source(BATTERY_POWERED);
            platform_io_power_up_oled(FALSE);
        }
        else
        {
            logic_power_set_power_source(USB_POWERED);
            platform_io_power_up_oled(TRUE);
        }
        
        sh1122_oled_on(&plat_oled_descriptor);
    }
}

/*! \fn     logic_device_bundle_update_start(BOOL from_debug_messages)
*   \brief  Function called when start updating the device graphics memory
*   \param  from_debug_messages Set to TRUE if this function was called from debug messages
*   \return RETURN_OK if we are allowed to start bundle update
*/
ret_type_te logic_device_bundle_update_start(BOOL from_debug_messages)
{
    logic_device_activity_detected();
    
    /* Function called from HID debug messages? */
    if (from_debug_messages != FALSE)
    {
        gui_screen_te current_screen = gui_dispatcher_get_current_screen();
        
        /* Depending on the mode we're currently in */
        if (current_screen == GUI_SCREEN_INVALID)        
        {
            /* If we are in invalid screen (variable not set), it means we don't have a bundle */
            return RETURN_OK;
        }
        else if (current_screen == GUI_SCREEN_INSERTED_INVALID)
        {
            /* Card inserted invalid: allow update and display notification */
            gui_dispatcher_set_current_screen(GUI_SCREEN_FW_FILE_UPDATE, TRUE, GUI_OUTOF_MENU_TRANSITION);
            gui_dispatcher_get_back_to_current_screen();
            return RETURN_OK;
        }
        else
        {
            return RETURN_NOK;
        }
    } 
    else
    {
        // TODO3
        return RETURN_OK;
    }
}

/*! \fn     logic_device_bundle_update_end(BOOL from_debug_messages)
*   \brief  Function called at the end of graphics upload
*   \param  from_debug_messages Set to TRUE if this function was called from debug messages
*/
void logic_device_bundle_update_end(BOOL from_debug_messages)
{
    /* If we are in invalid screen, it means we don't have a bundle */
    if (gui_dispatcher_get_current_screen() != GUI_SCREEN_INVALID)
    {
        if (from_debug_messages != FALSE)
        {        
            /* Refresh file system and font */
            custom_fs_init();
        
            /* Go to default screen */
            gui_dispatcher_set_current_screen(GUI_SCREEN_NINSERTED, TRUE, GUI_OUTOF_MENU_TRANSITION);
            gui_dispatcher_get_back_to_current_screen();
        } 
        else
        {
            // TODO3
        }
    }
}

/*! \fn     logic_device_set_state_changed(void)
*   \brief  Function called whenever the device state has changed
*/
void logic_device_set_state_changed(void)
{
    logic_device_state_changed = TRUE;
}


/*! \fn     logic_device_get_state_changed_and_reset_bool(void)
*   \brief  Fetch and reset state changed bool
*/
BOOL logic_device_get_state_changed_and_reset_bool(void)
{
    BOOL return_val = logic_device_state_changed;
    logic_device_state_changed = FALSE;
    return return_val;
}