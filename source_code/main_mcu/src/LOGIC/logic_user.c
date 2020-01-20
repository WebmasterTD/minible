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
/*!  \file     logic_user.c
*    \brief    General logic for user operations
*    Created:  16/02/2019
*    Author:   Mathieu Stephan
*/
#include <string.h>
#include "fido2_values_defines.h"
#include "smartcard_highlevel.h"
#include "logic_encryption.h"
#include "logic_bluetooth.h"
#include "logic_security.h"
#include "logic_database.h"
#include "gui_dispatcher.h"
#include "logic_aux_mcu.h"
#include "bearssl_block.h"
#include "comms_aux_mcu.h"
#include "logic_device.h"
#include "driver_timer.h"
#include "platform_io.h"
#include "gui_prompts.h"
#include "logic_user.h"
#include "custom_fs.h"
#include "logic_gui.h"
#include "nodemgmt.h"
#include "text_ids.h"
#include "utils.h"
#include "fido2.h"
#include "rng.h"
// User security preferences
uint16_t logic_user_cur_sec_preferences;


/*! \fn     logic_user_init_context(uint8_t user_id)
*   \brief  Initialize our user context
*   \param  user_id The user ID
*/
void logic_user_init_context(uint8_t user_id)
{
    uint16_t user_language;
    uint16_t user_usb_layout;
    uint16_t user_ble_layout;
    
    /* Initialize context and fetch user language & keyboard layout */
    nodemgmt_init_context(user_id, &logic_user_cur_sec_preferences, &user_language, &user_usb_layout, &user_ble_layout);
    custom_fs_set_current_language(utils_check_value_for_range(user_language, 0, custom_fs_get_number_of_languages()-1));
    custom_fs_set_current_keyboard_id(utils_check_value_for_range(user_usb_layout, 0, custom_fs_get_number_of_keyb_layouts()-1), TRUE);
    custom_fs_set_current_keyboard_id(utils_check_value_for_range(user_ble_layout, 0, custom_fs_get_number_of_keyb_layouts()-1), FALSE);
}

/*! \fn     logic_user_get_user_security_flags(void)
*   \brief  Get user security choices
*   \return The bitmask
*/
uint16_t logic_user_get_user_security_flags(void)
{
    return logic_user_cur_sec_preferences;
}

/*! \fn     logic_user_set_language(uint16_t language_id)
*   \brief  Set language for current user
*   \param  language_id User language ID
*/
void logic_user_set_language(uint16_t language_id)
{
    nodemgmt_store_user_language(language_id);
}

/*! \fn     logic_user_set_layout_id(uint16_t layout_id, BOOL usb_layout)
*   \brief  Set layout id for current user
*   \param  layout_id   User layout ID
*   \param  usb_layout  Set to TRUE to set USB layout
*/
void logic_user_set_layout_id(uint16_t layout_id, BOOL usb_layout)
{
    if (usb_layout == FALSE)
    {
        nodemgmt_store_user_ble_layout(layout_id);
    } 
    else
    {
        nodemgmt_store_user_layout(layout_id);
    }
}

/*! \fn     logic_user_get_current_user_id(void)
*   \brief  Get current user ID
*   \return User ID
*/
uint8_t logic_user_get_current_user_id(void)
{
    cpz_lut_entry_t* lut_entry_pt = logic_encryption_get_cur_cpz_lut_entry();
    
    if (lut_entry_pt != 0)
    {
        return lut_entry_pt->user_id;
    }
    else
    {
        /* No user set: invalid ID */
        return 0xFF;
    }
}

/*! \fn     logic_user_get_user_cards_cpz(uint8_t* buffer)
*   \brief  Get current user cards CPZ
*   \param  buffer      Where to store the user cards' CPZ
*/
void logic_user_get_user_cards_cpz(uint8_t* buffer)
{
    cpz_lut_entry_t* lut_entry_pt = logic_encryption_get_cur_cpz_lut_entry();
    
    if (lut_entry_pt != 0)
    {
        memcpy(buffer, lut_entry_pt->cards_cpz, MEMBER_SIZE(cpz_lut_entry_t, cards_cpz));
    }  
}

/*! \fn     logic_user_set_user_security_flag(uint8_t bitmask)
*   \brief  Add security flags to current user profile
*   \param  bitmask     Security flags bitmask
*/
void logic_user_set_user_security_flag(uint16_t bitmask)
{
    logic_device_set_state_changed();
    logic_user_cur_sec_preferences |= bitmask;
    nodemgmt_store_user_sec_preferences(logic_user_cur_sec_preferences);
}

/*! \fn     logic_user_clear_user_security_flag(uint16_t bitmask)
*   \brief  Clear security flags to current user profile
*   \param  bitmask     Security flags bitmask
*/
void logic_user_clear_user_security_flag(uint16_t bitmask)
{
    logic_device_set_state_changed();
    logic_user_cur_sec_preferences &= ~bitmask;
    nodemgmt_store_user_sec_preferences(logic_user_cur_sec_preferences);
}

/*! \fn     logic_user_create_new_user(volatile uint16_t* pin_code, uint8_t* provisioned_key, BOOL simple_mode)
*   \brief  Add a new user with a new smart card
*   \param  pin_code            The new pin code
*   \param  provisioned_key     If different than 0, provisioned aes key
*   \note   provisioned_key contents will be destroyed!
*   \return success or not
*/
ret_type_te logic_user_create_new_user(volatile uint16_t* pin_code, uint8_t* provisioned_key, BOOL simple_mode)
{    
    // When inserting a new user and a new card, we need to setup the following elements
    // - AES encryption key, stored in the smartcard
    // - AES next available CTR, stored in the user profile
    // - AES nonce, stored in the MCU flash along with the user ID
    // - Smartcard CPZ, randomly generated and stored in our MCU flash along with user id & nonce
    uint8_t temp_buffer[AES_KEY_LENGTH/8];
    uint8_t new_user_id;
    
    /* Check if there actually is an available slot */
    if (custom_fs_get_nb_free_cpz_lut_entries(&new_user_id) == 0)
    {
        return RETURN_NOK;
    }
    
    /* Setup user profile in MCU Flash */
    cpz_lut_entry_t user_profile;
    user_profile.user_id = new_user_id;
    
    /* Nonce & Cards CPZ: random numbers */
    rng_fill_array(user_profile.cards_cpz, sizeof(user_profile.cards_cpz));
    rng_fill_array(user_profile.nonce, sizeof(user_profile.nonce));
    
    /* Reserved field: set to 0 */
    memset(user_profile.reserved, 0, sizeof(user_profile.reserved));
    
    /* Setup user profile in external flash */
    if (simple_mode == FALSE)
    {
        nodemgmt_format_user_profile(new_user_id, 0xFFFF & (~USER_SEC_FLG_BLE_ENABLED), custom_fs_get_current_language_id(), custom_fs_get_recommended_layout_for_current_language());
    }
    else
    {
        nodemgmt_format_user_profile(new_user_id, 0, custom_fs_get_current_language_id(), custom_fs_get_recommended_layout_for_current_language());
    }
    
    /* Initialize nodemgmt context */
    logic_user_init_context(new_user_id);
    
    /* Write card CPZ */
    smartcard_highlevel_write_protected_zone(user_profile.cards_cpz);
    
    /* Write card random AES key */
    rng_fill_array(temp_buffer, sizeof(temp_buffer));
    if (smartcard_highlevel_write_aes_key(temp_buffer) != RETURN_OK)
    {
        return RETURN_NOK;
    }
    
    /* Use provisioned key? */
    if (provisioned_key != 0)
    {        
        /* Set flag in user profile */
        user_profile.use_provisioned_key_flag = CUSTOM_FS_PROV_KEY_FLAG;
        
        /* Buffer for empty ctr */
        uint8_t temp_ctr[AES256_CTR_LENGTH/8];
        memset(temp_ctr, 0, sizeof(temp_ctr));    
        
        /* Use card AES key to encrypt provisioned key */
        br_aes_ct_ctrcbc_keys temp_aes_context;
        br_aes_ct_ctrcbc_init(&temp_aes_context, temp_buffer, AES_KEY_LENGTH/8);
        br_aes_ct_ctrcbc_ctr(&temp_aes_context, (void*)temp_ctr, (void*)provisioned_key, AES_KEY_LENGTH/8);
        
        /* Store encrypted provisioned key in user profile */
        memcpy(user_profile.provisioned_key, provisioned_key, AES_KEY_LENGTH/8);
        
        /* Cleanup */
        memset((void*)&temp_aes_context, 0, sizeof(temp_aes_context));
        memset(provisioned_key, 0, AES_KEY_LENGTH/8);
    }
    else
    {
        user_profile.use_provisioned_key_flag = 0;
        rng_fill_array(user_profile.provisioned_key, sizeof(user_profile.provisioned_key));
    }
    
    /* Write down user profile. Will return OK as we've done the availability check before */
    custom_fs_store_cpz_entry(&user_profile, new_user_id);
    
    /* Initialize encryption context */
    cpz_lut_entry_t* cpz_stored_entry;
    custom_fs_get_cpz_lut_entry(user_profile.cards_cpz, &cpz_stored_entry);
    logic_encryption_init_context(temp_buffer, cpz_stored_entry);
    
    /* Erase AES key from memory */    
    memset(temp_buffer, 0, sizeof(temp_buffer));
    
    /* Write new pin code */
    smartcard_highlevel_write_security_code(pin_code);
    
    /* Remove power to smartcard */
    platform_io_smc_remove_function();

    /* Wait a few ms */
    timer_delay_ms(200);

    /* Reconnect it, test the card */
    if ((smartcard_highlevel_card_detected_routine() == RETURN_MOOLTIPASS_USER) && (smartcard_highlevel_check_hidden_aes_key_contents() == RETURN_OK) && (smartcard_high_level_mooltipass_card_detected_routine(pin_code) == RETURN_MOOLTIPASS_4_TRIES_LEFT))
    {
        return RETURN_OK;
    }
    else
    {
        /* Reset smartcard and delete just created user */
        smartcard_high_level_mooltipass_card_detected_routine(pin_code);
        smartcard_highlevel_erase_smartcard();
        custom_fs_detele_user_cpz_lut_entry(new_user_id);

        // Report fail
        return RETURN_NOK;
    }
}

/*! \fn     logic_user_check_credential(cust_char_t* service, cust_char_t* login, cust_char_t* password)
*   \brief  Check if credential exists
*   \param  service     Pointer to service string
*   \param  login       Pointer to login string
*   \param  password    Pointer to password string
*   \return success or not
*/
RET_TYPE logic_user_check_credential(cust_char_t* service, cust_char_t* login, cust_char_t* password)
{
    cust_char_t encrypted_password[MEMBER_SIZE(child_cred_node_t,password)/sizeof(cust_char_t)];
    uint8_t temp_cred_ctr[MEMBER_SIZE(nodemgmt_profile_main_data_t, current_ctr)];
    BOOL prev_gen_credential_flag = FALSE;
    
    /* Smartcard present and unlocked? */
    if (logic_security_is_smc_inserted_unlocked() == FALSE)
    {
        return -1;
    }
    
    /* Does service already exist? */
    uint16_t parent_address = logic_database_search_service(service, COMPARE_MODE_MATCH, TRUE, 0);
    
    /* Service doesn't exist, deny request with a variable timeout for privacy concerns */
    if (parent_address == NODE_ADDR_NULL)
    {
        return RETURN_NOK;
    }

    /* Check if child actually exists */
    uint16_t child_address = logic_database_search_login_in_service(parent_address, login, TRUE);
            
    /* Check for existing login */
    if (child_address == NODE_ADDR_NULL)
    {
        return RETURN_NOK;
    }
    
    /* Fetch password */
    logic_database_fetch_encrypted_password(child_address, (uint8_t*)encrypted_password, temp_cred_ctr, &prev_gen_credential_flag);
        
    /* User approved, decrypt password */
    logic_encryption_ctr_decrypt((uint8_t*)encrypted_password, temp_cred_ctr, MEMBER_SIZE(child_cred_node_t, password), prev_gen_credential_flag);
        
    /* If old generation password, convert it to unicode */
    if (prev_gen_credential_flag != FALSE)
    {
        _Static_assert(MEMBER_SIZE(child_cred_node_t, password) >= NODEMGMT_OLD_GEN_ASCII_PWD_LENGTH*2 + 2, "Backward compatibility problem");
        utils_ascii_to_unicode((uint8_t*)encrypted_password, NODEMGMT_OLD_GEN_ASCII_PWD_LENGTH);
    }

    /* Finally do the comparison */
    if (utils_custchar_strncmp(encrypted_password, password, sizeof(encrypted_password)/sizeof(cust_char_t)) == 0)
    {
        memset(encrypted_password, 0, sizeof(encrypted_password));
        return RETURN_OK;
    } 
    else
    {
        memset(encrypted_password, 0, sizeof(encrypted_password));
        return RETURN_NOK;
    }
}

/*! \fn     logic_user_store_webauthn_credential(cust_char_t* rp_id, uint8_t* user_handle, cust_char_t* user_name, cust_char_t* display_name, uint8_t* private_key, uint8_t* credential_id)
*   \brief  Generate and store new webauthn credential
*   \param  rp_id           Pointer to relying party string
*   \param  user_handle     Opaque byte sequence of 64 bytes.
*   \param  user_name       Pointer to user name string
*   \param  display_name    Pointer to display name string
*   \param  private_key     32 bytes private key
*   \param  credential_id   Pointer to credential ID
*   \return FIDO2 success code
*   \note   This function doesn't parse aux MCU messages in order to safely use aux mcu received message
*/
fido2_return_code_te logic_user_store_webauthn_credential(cust_char_t* rp_id, uint8_t* user_handle, cust_char_t* user_name, cust_char_t* display_name, uint8_t* private_key, uint8_t* credential_id)
{
    uint8_t encrypted_private_key[MEMBER_SIZE(child_webauthn_node_t, private_key)];
    uint8_t temp_cred_ctr_val[MEMBER_SIZE(child_webauthn_node_t, ctr)];
    cust_char_t* three_line_prompt_2;
    
    /* Sanity checks */
    _Static_assert(FIDO2_CREDENTIAL_ID_LENGTH == MEMBER_SIZE(child_webauthn_node_t, credential_id), "Invalid FIDO2 credential id length");
    
    /* Smartcard present and unlocked? */
    if (logic_security_is_smc_inserted_unlocked() == FALSE)
    {
        return FIDO2_USER_NOT_PRESENT;
    }
    
    /* Does service already exist? */
    uint16_t parent_address = logic_database_search_service(rp_id, COMPARE_MODE_MATCH, TRUE, NODEMGMT_WEBAUTHN_CRED_TYPE_ID);
    uint16_t child_address = NODE_ADDR_NULL;
    
    /* If service exist, does user_handle exist? */
    if (parent_address != NODE_ADDR_NULL)
    {
        child_address = logic_database_search_webauthn_userhandle_in_service(parent_address, user_handle);
        
        /* If it does, don't overwrite it... */
        if (child_address != NODE_ADDR_NULL)
        {
            return FIDO2_OPERATION_DENIED;
        }
    }

    /* Prepare prompt text */
    custom_fs_get_string_from_file(ADD_CRED_TEXT_ID, &three_line_prompt_2, TRUE);
    confirmationText_t conf_text_3_lines = {.lines[0]=rp_id, .lines[1]=three_line_prompt_2, .lines[2]=user_name};
        
    /* Request user approval */
    mini_input_yes_no_ret_te prompt_return = gui_prompts_ask_for_confirmation(3, &conf_text_3_lines, TRUE, FALSE);
    gui_dispatcher_get_back_to_current_screen();
        
    /* Did the user approve? */
    if (prompt_return != MINI_INPUT_RET_YES)
    {
        return FIDO2_OPERATION_DENIED;
    }
    
    /* If needed, add service */
    if (parent_address == NODE_ADDR_NULL)
    {
        parent_address = logic_database_add_service(rp_id, SERVICE_CRED_TYPE, NODEMGMT_WEBAUTHN_CRED_TYPE_ID);
        
        /* Check for operation success */
        if (parent_address == NODE_ADDR_NULL)
        {
            return FIDO2_STORAGE_EXHAUSTED;
        }
    }
    
    /* Copy private key into array */
    memcpy(encrypted_private_key, private_key, sizeof(encrypted_private_key));
        
    /* CTR encrypt key */
    logic_encryption_ctr_encrypt(encrypted_private_key, sizeof(encrypted_private_key), temp_cred_ctr_val);
    
    /* Create new webauthn credential */
    RET_TYPE temp_ret = logic_database_add_webauthn_credential_for_service(parent_address, user_handle, user_name, display_name, encrypted_private_key, temp_cred_ctr_val, credential_id);
    
    /* Correct return depending on credential add result */
    if (temp_ret == RETURN_OK)
    {
        return FIDO2_SUCCESS;
    } 
    else
    {
        return FIDO2_STORAGE_EXHAUSTED;
    }
}

/*! \fn     logic_user_store_credential(cust_char_t* service, cust_char_t* login, cust_char_t* desc, cust_char_t* third, cust_char_t* password)
*   \brief  Store new credential
*   \param  service     Pointer to service string
*   \param  login       Pointer to login string
*   \param  desc        Pointer to description string, or 0 if not specified
*   \param  third       Pointer to arbitrary third field, or 0 if not specified
*   \param  password    Pointer to password string, or 0 if not specified
*   \return success or not
*   \note   This function doesn't parse aux MCU messages in order to safely use aux mcu received message
*/
RET_TYPE logic_user_store_credential(cust_char_t* service, cust_char_t* login, cust_char_t* desc, cust_char_t* third, cust_char_t* password)
{
    cust_char_t encrypted_password[MEMBER_SIZE(child_cred_node_t, password)/sizeof(cust_char_t)];
    uint8_t temp_cred_ctr_val[MEMBER_SIZE(nodemgmt_profile_main_data_t, current_ctr)];
    
    /* Smartcard present and unlocked? */
    if (logic_security_is_smc_inserted_unlocked() == FALSE)
    {
        return RETURN_NOK;
    }
    
    /* Does service already exist? */
    uint16_t parent_address = logic_database_search_service(service, COMPARE_MODE_MATCH, TRUE, NODEMGMT_STANDARD_CRED_TYPE_ID);
    uint16_t child_address = NODE_ADDR_NULL;
    
    /* If service exist, does login exist? */
    if (parent_address != NODE_ADDR_NULL)
    {
        child_address = logic_database_search_login_in_service(parent_address, login, TRUE);
    }

    /* Special case: in MMM and user chose to not be prompted */
    if ((logic_security_is_management_mode_set() == FALSE) || ((logic_user_get_user_security_flags() & USER_SEC_FLG_CRED_SAVE_PROMPT_MMM) != 0))
    {        
        /* Prepare prompt text */
        cust_char_t* three_line_prompt_2;
        if (child_address == NODE_ADDR_NULL)
        {
            custom_fs_get_string_from_file(ADD_CRED_TEXT_ID, &three_line_prompt_2, TRUE);
        }
        else
        {
            custom_fs_get_string_from_file(CHANGE_PWD_TEXT_ID, &three_line_prompt_2, TRUE);
        }
        confirmationText_t conf_text_3_lines = {.lines[0]=service, .lines[1]=three_line_prompt_2, .lines[2]=login};
        
        /* Request user approval */
        mini_input_yes_no_ret_te prompt_return = gui_prompts_ask_for_confirmation(3, &conf_text_3_lines, TRUE, FALSE);
        gui_dispatcher_get_back_to_current_screen();
        
        /* Did the user approve? */
        if (prompt_return != MINI_INPUT_RET_YES)
        {
            return RETURN_NOK;
        }
    }
    
    /* If needed, add service */
    if (parent_address == NODE_ADDR_NULL)
    {
        parent_address = logic_database_add_service(service, SERVICE_CRED_TYPE, NODEMGMT_STANDARD_CRED_TYPE_ID);
        
        /* Check for operation success */
        if (parent_address == NODE_ADDR_NULL)
        {
            return RETURN_NOK;
        }
    }
    
    /* Fill RNG array with random numbers */
    rng_fill_array((uint8_t*)encrypted_password, sizeof(encrypted_password));
    
    /* Password provided? */
    if (password != 0)
    {
        /* Copy password into array, no need to terminate it given the underlying database model */
        utils_strncpy(encrypted_password, password, sizeof(encrypted_password)/sizeof(cust_char_t));
        
        /* CTR encrypt password */
        logic_encryption_ctr_encrypt((uint8_t*)encrypted_password, sizeof(encrypted_password), temp_cred_ctr_val);
    }
    else if (child_address == NODE_ADDR_NULL)
    {
        /* New credential but password somehow not specified */
        encrypted_password[0] = 0;
        
        /* CTR encrypt password */
        logic_encryption_ctr_encrypt((uint8_t*)encrypted_password, sizeof(encrypted_password), temp_cred_ctr_val);
    }
    
    /* Update existing login or create new one? */
    if (child_address != NODE_ADDR_NULL)
    {
        if (password != 0)
        {
            logic_database_update_credential(child_address, desc, third, (uint8_t*)encrypted_password, temp_cred_ctr_val);
        } 
        else
        {
            logic_database_update_credential(child_address, desc, third, 0, 0);
        }
        return RETURN_OK;
    }
    else
    {
        return logic_database_add_credential_for_service(parent_address, login, desc, third, (uint8_t*)encrypted_password, temp_cred_ctr_val);
    }
}

/*! \fn     logic_user_get_webauthn_credential_key_for_rp(cust_char_t* rp_id, uint8_t* credential_id, uint8_t* private_key, uint32_t* count, uint8_t** credential_id_allow_list, uint16_t credential_id_allow_list_length)
*   \brief  Get credential private key for a possible credential for a relying party
*   \param  rp_id                           Pointer to relying party string
*   \param  credential_id                   16B buffer to where to store the credential id
*   \param  private_key                     32B buffer to where to store the private key
*   \param  count                           Pointer to uint32_t to store authentication count
*   \param  credential_id_allow_list        If credential_id_allow_list_length != 0, list of credential ids we allow
*   \param  credential_id_allow_list_length Length of the credential allow list
*   \return success status
*/
RET_TYPE logic_user_get_webauthn_credential_key_for_rp(cust_char_t* rp_id, uint8_t* credential_id, uint8_t* private_key, uint32_t* count, uint8_t** credential_id_allow_list, uint16_t credential_id_allow_list_length)
{
    uint8_t temp_cred_ctr[MEMBER_SIZE(child_webauthn_node_t, ctr)];
    
    /* TODO2: allow an allow list that has more than 1, which requires extra code on the GUI as it isn't as simple as listing all children nodes */
    /* However, I'm not sure why this would happen, as the RP would need to keep track of all aliases of a given user... */
    
    /* Copy strings locally */
    cust_char_t rp_id_copy[MEMBER_ARRAY_SIZE(parent_cred_node_t, service)];
    cust_char_t temp_user_name[MEMBER_ARRAY_SIZE(child_webauthn_node_t, user_name)+1];
    utils_strncpy(rp_id_copy, rp_id, MEMBER_ARRAY_SIZE(parent_cred_node_t, service));
    rp_id_copy[MEMBER_ARRAY_SIZE(parent_cred_node_t, service)-1] = 0;
    memset(temp_user_name, 0, sizeof(temp_user_name));
    
    /* Switcheroo */
    rp_id = rp_id_copy;
    
    /* Smartcard present and unlocked? */
    if (logic_security_is_smc_inserted_unlocked() == FALSE)
    {
        return -1;
    }
    
    /* Does service already exist? */
    uint16_t parent_address = logic_database_search_service(rp_id, COMPARE_MODE_MATCH, TRUE, NODEMGMT_WEBAUTHN_CRED_TYPE_ID);
    uint16_t child_address = NODE_ADDR_NULL;
    
    /* Service doesn't exist, deny request with a variable timeout for privacy concerns */
    if (parent_address == NODE_ADDR_NULL)
    {
        /* From 1s to 3s */
        timer_delay_ms(1000 + (rng_get_random_uint16_t()&0x07FF));
        return RETURN_NOK;
    }
    
    /* See how many credentials there are for this service */
    uint16_t nb_logins_for_cred = logic_database_get_number_of_creds_for_service(parent_address, &child_address, FALSE);
    
    /* Check if wanted credential id has been specified or if there's only one credential for that service */
    if ((credential_id_allow_list_length == 1) || (nb_logins_for_cred == 1))
    {
        /* Login specified? look for it */
        if (credential_id_allow_list_length != 0)
        {
            child_address = logic_database_search_webauthn_credential_id_in_service(parent_address, credential_id_allow_list[0]);
            
            /* Check for existing login */
            if (child_address == NODE_ADDR_NULL)
            {
                /* From 3s to 7s */
                timer_delay_ms(3000 + (rng_get_random_uint16_t()&0x0FFF));
                return RETURN_NOK;
            }
        }
        
        /* Fetch username for that credential id, username is already 0 terminated by code above */
        logic_database_get_webauthn_username_for_address(child_address, temp_user_name);
        
        /* If user specified to be prompted for login confirmation */
        if ((logic_user_get_user_security_flags() & USER_SEC_FLG_LOGIN_CONF) != 0)
        {
            /* Prepare prompt message */
            cust_char_t* three_line_prompt_2;
            custom_fs_get_string_from_file(SEND_CREDS_FOR_TEXT_ID, &three_line_prompt_2, TRUE);
            confirmationText_t conf_text_3_lines = {.lines[0]=rp_id, .lines[1]=three_line_prompt_2, .lines[2]=temp_user_name};

            /* Request user approval */
            mini_input_yes_no_ret_te prompt_return = gui_prompts_ask_for_confirmation(3, &conf_text_3_lines, TRUE, TRUE);
            gui_dispatcher_get_back_to_current_screen();

            /* Did the user approve? */
            if (prompt_return != MINI_INPUT_RET_YES)
            {
                return RETURN_NOK;
            }
        }
        else
        {
            /* Prepare notification message */
            cust_char_t* three_line_notif_2;
            custom_fs_get_string_from_file(LOGGING_WITH_TEXT_ID, &three_line_notif_2, TRUE);
            confirmationText_t notif_text_3_lines = {.lines[0]=rp_id, .lines[1]=three_line_notif_2, .lines[2]=temp_user_name};

            /* 3 lines notification website / logging you in with / username */
            gui_prompts_display_3line_information_on_screen(&notif_text_3_lines, DISP_MSG_INFO);

            /* Set information screen, do not call get back to current screen as screen is already updated */
            gui_dispatcher_set_current_screen(GUI_SCREEN_LOGIN_NOTIF, FALSE, GUI_INTO_MENU_TRANSITION);
        }
        
        /* Fetch webauthn data */
        logic_database_get_webauthn_data_for_address(child_address, credential_id, private_key, count, temp_cred_ctr);
        
        /* User approved, decrypt key */
        logic_encryption_ctr_decrypt(private_key, temp_cred_ctr, MEMBER_SIZE(child_webauthn_node_t, private_key), FALSE);
        
        return RETURN_OK;
    }
    else
    {
        /* 3 cases: no login, 1 login, several logins */
        if (nb_logins_for_cred == 0)
        {
            /* From 1s to 3s */
            timer_delay_ms(1000 + (rng_get_random_uint16_t()&0x07FF));
            return RETURN_NOK;
        }
        else
        {
            /* 2 children or more, as 1 is tackled in the previous if */
            
            /* Here chosen_login_addr already is populated with the first node... isn't that pretty? */
            mini_input_yes_no_ret_te display_prompt_return = gui_prompts_ask_for_login_select(parent_address, &child_address);
            if (display_prompt_return != MINI_INPUT_RET_YES)
            {
                child_address = NODE_ADDR_NULL;
            }
            gui_dispatcher_get_back_to_current_screen();
            
            /* So.... what did the user select? */
            if (child_address == NODE_ADDR_NULL)
            {
                return RETURN_NOK;
            }
            else
            {
                /* Fetch webauthn data */
                logic_database_get_webauthn_data_for_address(child_address, credential_id, private_key, count, temp_cred_ctr);
                
                /* User approved, decrypt key */
                logic_encryption_ctr_decrypt(private_key, temp_cred_ctr, MEMBER_SIZE(child_webauthn_node_t, private_key), FALSE);
                
                return RETURN_OK;
            }
        }
    }    
}

/*! \fn     logic_user_usb_get_credential(cust_char_t* service, cust_char_t* login, hid_message_t* send_msg)
*   \brief  Get credential for service
*   \param  service     Pointer to service string
*   \param  login       Pointer to login string, or 0 if not specified
*   \param  send_msg    Pointer to where to store our answer
*   \return payload size or -1 if error
*/
int16_t logic_user_usb_get_credential(cust_char_t* service, cust_char_t* login, hid_message_t* send_msg)
{
    uint8_t temp_cred_ctr[MEMBER_SIZE(nodemgmt_profile_main_data_t, current_ctr)];
    BOOL prev_gen_credential_flag = FALSE;
    
    /* Copy strings locally */
    cust_char_t service_copy[MEMBER_ARRAY_SIZE(parent_cred_node_t, service)];
    cust_char_t login_copy[MEMBER_ARRAY_SIZE(child_cred_node_t, login)];
    utils_strncpy(service_copy, service, MEMBER_ARRAY_SIZE(parent_cred_node_t, service));
    service_copy[MEMBER_ARRAY_SIZE(parent_cred_node_t, service)-1] = 0;
    memset(login_copy, 0, sizeof(login_copy));
    
    /* Switcheroo */
    service = service_copy;
    if (login != 0)
    {
        utils_strncpy(login_copy, login, MEMBER_ARRAY_SIZE(child_cred_node_t, login));
        login_copy[MEMBER_ARRAY_SIZE(child_cred_node_t, login)-1] = 0;
        login = login_copy;
    }
    
    /* Smartcard present and unlocked? */
    if (logic_security_is_smc_inserted_unlocked() == FALSE)
    {
        return -1;
    }
    
    /* Does service already exist? */
    uint16_t parent_address = logic_database_search_service(service, COMPARE_MODE_MATCH, TRUE, NODEMGMT_STANDARD_CRED_TYPE_ID);
    uint16_t child_address = NODE_ADDR_NULL;
    
    /* Service doesn't exist, deny request with a variable timeout for privacy concerns */
    if (parent_address == NODE_ADDR_NULL)
    {
        /* From 1s to 3s */
        timer_delay_ms(1000 + (rng_get_random_uint16_t()&0x07FF));
        return -1;
    }    
    
    /* See how many credentials there are for this service */
    uint16_t nb_logins_for_cred = logic_database_get_number_of_creds_for_service(parent_address, &child_address, !logic_security_is_management_mode_set());
    
    /* Check if wanted login has been specified or if there's only one credential for that service */
    if ((login != 0) || (nb_logins_for_cred == 1))
    {
        /* Login specified? look for it */
        if (login != 0)
        {
            child_address = logic_database_search_login_in_service(parent_address, login, !logic_security_is_management_mode_set());
            
            /* Check for existing login */
            if (child_address == NODE_ADDR_NULL)
            {
                /* From 3s to 7s */
                timer_delay_ms(3000 + (rng_get_random_uint16_t()&0x0FFF));
                return -1;
            }
        }
        else
        {
            /* Only one login for current service, fetch it and store it locally */
            login = login_copy;
            logic_database_get_login_for_address(child_address, &login);
        }
        
        /* If user specified to be prompted for login confirmation */
        if ((logic_user_get_user_security_flags() & USER_SEC_FLG_LOGIN_CONF) != 0)
        {
            /* Prepare prompt message */
            cust_char_t* three_line_prompt_2;
            custom_fs_get_string_from_file(SEND_CREDS_FOR_TEXT_ID, &three_line_prompt_2, TRUE);
            confirmationText_t conf_text_3_lines = {.lines[0]=service, .lines[1]=three_line_prompt_2, .lines[2]=login};

            /* Request user approval */
            mini_input_yes_no_ret_te prompt_return = gui_prompts_ask_for_confirmation(3, &conf_text_3_lines, TRUE, TRUE);
            gui_dispatcher_get_back_to_current_screen();

            /* Did the user approve? */
            if (prompt_return != MINI_INPUT_RET_YES)
            {
                memset(send_msg->payload, 0, sizeof(send_msg->payload));
                return -1;
            }
        }
        else
        {
            /* Prepare notification message : contents of the TX message aren't accessed after this function return */
            cust_char_t* three_line_notif_2;
            if (logic_security_is_management_mode_set() != FALSE)
            {                
                custom_fs_get_string_from_file(ACCESS_TO_TEXT_ID, &three_line_notif_2, TRUE);
            } 
            else
            {
                custom_fs_get_string_from_file(LOGGING_WITH_TEXT_ID, &three_line_notif_2, TRUE);
            }
            confirmationText_t notif_text_3_lines = {.lines[0]=service, .lines[1]=three_line_notif_2, .lines[2]=login};

            /* 3 lines notification website / logging you in with / username */
            gui_prompts_display_3line_information_on_screen(&notif_text_3_lines, DISP_MSG_INFO);

            /* Set information screen, do not call get back to current screen as screen is already updated */
            gui_dispatcher_set_current_screen(GUI_SCREEN_LOGIN_NOTIF, FALSE, GUI_INTO_MENU_TRANSITION);
        }
        
        /* Get prefilled message */
        uint16_t return_payload_size_without_pwd = logic_database_fill_get_cred_message_answer(child_address, send_msg, temp_cred_ctr, &prev_gen_credential_flag);
        
        /* User approved, decrypt password */
        logic_encryption_ctr_decrypt((uint8_t*)&(send_msg->get_credential_answer.concatenated_strings[send_msg->get_credential_answer.password_index]), temp_cred_ctr, MEMBER_SIZE(child_cred_node_t, password), prev_gen_credential_flag);
        
        /* If old generation password, convert it to unicode */
        if (prev_gen_credential_flag != FALSE)
        {
            _Static_assert(MEMBER_SIZE(child_cred_node_t, password) >= NODEMGMT_OLD_GEN_ASCII_PWD_LENGTH*2 + 2, "Backward compatibility problem");
            utils_ascii_to_unicode((uint8_t*)&(send_msg->get_credential_answer.concatenated_strings[send_msg->get_credential_answer.password_index]), NODEMGMT_OLD_GEN_ASCII_PWD_LENGTH);
        }
                
        /* Get password length */
        uint16_t pwd_length = utils_strlen(&(send_msg->get_credential_answer.concatenated_strings[send_msg->get_credential_answer.password_index]));
        
        /* Compute payload size */
        uint16_t return_payload_size = return_payload_size_without_pwd + (pwd_length + 1)*sizeof(cust_char_t);
        
        /* Return payload size */
        send_msg->payload_length = return_payload_size;
        return return_payload_size;
    }
    else
    {        
        /* 3 cases: no login, 1 login, several logins */
        if (nb_logins_for_cred == 0)
        {
            /* From 1s to 3s */
            timer_delay_ms(1000 + (rng_get_random_uint16_t()&0x07FF));
            return -1;
        }
        else
        {
            /* 2 children or more, as 1 is tackled in the previous if */
            
            /* Here chosen_login_addr already is populated with the first node... isn't that pretty? */
            mini_input_yes_no_ret_te display_prompt_return = gui_prompts_ask_for_login_select(parent_address, &child_address);
            if (display_prompt_return != MINI_INPUT_RET_YES)
            {
                child_address = NODE_ADDR_NULL;
            }
            gui_dispatcher_get_back_to_current_screen();
            
            /* So.... what did the user select? */
            if (child_address == NODE_ADDR_NULL)
            {
                return -1;
            }
            else
            {
                /* Get prefilled message */
                uint16_t return_payload_size_without_pwd = logic_database_fill_get_cred_message_answer(child_address, send_msg, temp_cred_ctr, &prev_gen_credential_flag);
                
                /* User approved, decrypt password */
                logic_encryption_ctr_decrypt((uint8_t*)&(send_msg->get_credential_answer.concatenated_strings[send_msg->get_credential_answer.password_index]), temp_cred_ctr, MEMBER_SIZE(child_cred_node_t, password), prev_gen_credential_flag);
                
                /* If old generation password, convert it to unicode */
                if (prev_gen_credential_flag != FALSE)
                {
                    _Static_assert(MEMBER_SIZE(child_cred_node_t, password) >= NODEMGMT_OLD_GEN_ASCII_PWD_LENGTH*2 + 2, "Backward compatibility problem");
                    utils_ascii_to_unicode((uint8_t*)&(send_msg->get_credential_answer.concatenated_strings[send_msg->get_credential_answer.password_index]), NODEMGMT_OLD_GEN_ASCII_PWD_LENGTH);
                }
                
                /* Get password length */
                uint16_t pwd_length = utils_strlen(&(send_msg->get_credential_answer.concatenated_strings[send_msg->get_credential_answer.password_index]));
                
                /* Compute payload size */
                uint16_t return_payload_size = return_payload_size_without_pwd + (pwd_length + 1)*sizeof(cust_char_t);
                
                /* Return payload size */
                send_msg->payload_length = return_payload_size;
                return return_payload_size;                
            }
        }
    }    
}

/*! \fn     logic_user_manual_select_login(void)
*   \brief  Logic for finding a given login
*/
void logic_user_manual_select_login(void)
{
    uint16_t chosen_service_addr = nodemgmt_get_starting_parent_addr_for_category(NODEMGMT_STANDARD_CRED_TYPE_ID);
    uint16_t chosen_login_addr = NODE_ADDR_NULL;
    BOOL only_password_prompt = FALSE;
    BOOL usb_interface_output = TRUE;
    uint16_t nb_logins_for_cred = 0;
    uint16_t state_machine = 0;

    while (TRUE)
    {
        if (state_machine == 0)
        {
            /* Ask user to select a service */
            chosen_service_addr = gui_prompts_service_selection_screen(chosen_service_addr);

            /* No service was chosen or card removed */
            if (chosen_service_addr == NODE_ADDR_NULL)
            {
                return;
            }

            /* Continue */
            nb_logins_for_cred = UINT16_MAX;
            state_machine++;
        }
        else if (state_machine == 1)
        {
            /* See how many credentials there are for this service, only if we haven't done this before (we may be walking back...) */
            if (nb_logins_for_cred == UINT16_MAX)
            {
                nb_logins_for_cred = logic_database_get_number_of_creds_for_service(chosen_service_addr, &chosen_login_addr, TRUE);
            }

            /* More than one login */
            if (nb_logins_for_cred != 1)
            {
                /* Here chosen_login_addr is populated with the first node... isn't that pretty? */
                mini_input_yes_no_ret_te display_prompt_return = gui_prompts_ask_for_login_select(chosen_service_addr, &chosen_login_addr);
                if (display_prompt_return == MINI_INPUT_RET_BACK)
                {
                    state_machine--;
                }
                else if (display_prompt_return == MINI_INPUT_RET_YES)
                {
                    state_machine++;
                }
                else
                {
                    return;
                }
            }
            else
            {
                /* Card removed, user going back... exit */
                if (chosen_login_addr == NODE_ADDR_NULL)
                {
                    return;
                }
                else
                {
                    state_machine++;
                }                
            }
        }
        else if (state_machine == 2)
        {
            /* Ask the user permission to enter login / password, check for back action */
            ret_type_te user_prompt_return = logic_user_ask_for_credentials_keyb_output(chosen_service_addr, chosen_login_addr, only_password_prompt, &usb_interface_output);  
            
            /* Ask the user permission to enter login / password, check for back action */
            if (user_prompt_return == RETURN_BACK)
            {
                /* Depending on number of child nodes, go back in history */
                if (nb_logins_for_cred == 1)
                {
                    /* Go back to service selection */
                    only_password_prompt = FALSE;
                    state_machine = 0;
                } 
                else
                {
                    /* Go back to login selection */
                    only_password_prompt = FALSE;
                    state_machine--;
                }
            }
            else if (user_prompt_return == RETURN_NOK)
            {
                /* We're either not connected to anything or user denied prompts to type credentials... ask him for credentials display */
                state_machine++;
            }
            else
            {
                return;
            }
        }
        else if (state_machine == 3)
        {
            // Fetch parent node to prepare prompt text
            _Static_assert(sizeof(child_node_t) >= sizeof(parent_node_t), "Invalid buffer reuse");
            child_node_t temp_cnode;
            parent_node_t* temp_pnode_pt = (parent_node_t*)&temp_cnode;
            nodemgmt_read_parent_node(chosen_service_addr, temp_pnode_pt, TRUE);
            
            // Ask the user if he wants to display credentials on screen
            cust_char_t* display_cred_prompt_text;
            custom_fs_get_string_from_file(QPROMPT_SNGL_DISP_CRED_TEXT_ID, &display_cred_prompt_text, TRUE);
            confirmationText_t prompt_object = {.lines[0] = temp_pnode_pt->cred_parent.service, .lines[1] = display_cred_prompt_text};
            mini_input_yes_no_ret_te display_prompt_return = gui_prompts_ask_for_confirmation(2, &prompt_object, FALSE, TRUE);
            
            if (display_prompt_return == MINI_INPUT_RET_BACK)
            {
                /* If we aren't connected to anything, don't ask to type again and go back in history */
                if ((logic_bluetooth_get_state() != BT_STATE_CONNECTED) && (logic_aux_mcu_is_usb_enumerated() == FALSE))
                {
                    /* Depending on number of child nodes, go back in history */
                    if (nb_logins_for_cred == 1)
                    {
                        /* Go back to service selection */
                        state_machine = 0;
                    }
                    else
                    {
                        /* Go back to login selection */
                        state_machine = 1;
                    }
                }
                else
                {
                    /* Otherwise go back to ask to type password */
                    only_password_prompt = TRUE;
                    state_machine--;
                }
            }
            else if (display_prompt_return == MINI_INPUT_RET_YES)
            {
                nodemgmt_read_cred_child_node(chosen_login_addr, (child_cred_node_t*)&temp_cnode);
                logic_gui_display_login_password((child_cred_node_t*)&temp_cnode);
                memset(&temp_cnode, 0, sizeof(temp_cnode));
                return;
            }
            else
            {
                return;
            }
        }            
    }
}

/*! \fn     logic_user_ask_for_credentials_keyb_output(uint16_t parent_address, uint16_t child_address, BOOL only_pwd_prompt, BOOL* usb_selected)
*   \brief  Ask the user to enter the login & password of a given service
*   \param  parent_address  Address of the parent
*   \param  child_address   Address of the child
*   \param  only_pwd_prompt Boolean to set if we only want the password prompt (used in case of going back)
*   \param  usb_selected    Pointer to a boolean, used internally to know if the user selected USB interface
*   \return RETURN_OK if a login or password was typed or if the card was removed, RETURN_BACK if the user wants to come back, RETURN_NOK if the user denied both typing prompts or device isn't connected to anything
*/
RET_TYPE logic_user_ask_for_credentials_keyb_output(uint16_t parent_address, uint16_t child_address, BOOL only_pwd_prompt, BOOL* usb_selected)
{
    _Static_assert(MEMBER_ARRAY_SIZE(keyboard_type_message_t,keyboard_symbols) > MEMBER_ARRAY_SIZE(child_cred_node_t,password)+1+1, "Can't describe all chars for password");
    _Static_assert(MEMBER_ARRAY_SIZE(keyboard_type_message_t,keyboard_symbols) > MEMBER_ARRAY_SIZE(child_cred_node_t,login)+1, "Can't describe all chars for login");
    uint16_t interface_id = (*usb_selected == FALSE)? 1:0;
    BOOL login_or_password_typed = FALSE;
    aux_mcu_message_t* temp_rx_message;
    child_cred_node_t temp_cnode;
    BOOL could_type_all_symbols;
    parent_node_t temp_pnode;
    
    /* Are we at least connected to anything? */
    if ((logic_bluetooth_get_state() != BT_STATE_CONNECTED) && (logic_aux_mcu_is_usb_enumerated() == FALSE))
    {
        return RETURN_NOK;
    }

    /* Read nodes */
    nodemgmt_read_parent_node(parent_address, &temp_pnode, TRUE);
    nodemgmt_read_cred_child_node(child_address, &temp_cnode);

    /* Prepare first line display (service / user), store it in the service field. fields are 0 terminated by previous calls */
    if (utils_strlen(temp_pnode.cred_parent.service) + utils_strlen(temp_cnode.login) + 4 <= (uint16_t)MEMBER_ARRAY_SIZE(parent_cred_node_t, service))
    {
        uint16_t parent_length = utils_strlen(temp_pnode.cred_parent.service);
        temp_pnode.cred_parent.service[parent_length] = ' ';
        temp_pnode.cred_parent.service[parent_length+1] = '/';
        temp_pnode.cred_parent.service[parent_length+2] = ' ';
        utils_strcpy(&temp_pnode.cred_parent.service[parent_length+3], temp_cnode.login);
    }
    
    /* Prepare prompt and state machine */
    uint16_t state_machine = 0;
    cust_char_t* two_line_prompt_2;
    confirmationText_t conf_text_2_lines = {.lines[0]=temp_pnode.cred_parent.service, .lines[1]=two_line_prompt_2};
        
    /* If only password prompt was queried, go to dedicated state machine */
    if (only_pwd_prompt != FALSE)
    {
        state_machine = 2;
    }
    
    while (TRUE)
    {
        if (state_machine == 0)
        {
            /* How many interfaces connected? */
            if ((logic_bluetooth_get_state() == BT_STATE_CONNECTED) && (logic_aux_mcu_is_usb_enumerated() != FALSE))
            {
                /* Both interface connected, ask user for selection */
                mini_input_yes_no_ret_te select_inteface_prompt_return = gui_prompts_ask_for_one_line_confirmation(SELECT_OUT_INTEFACE_TEXT_ID, FALSE, TRUE, *usb_selected);
                
                if (select_inteface_prompt_return == MINI_INPUT_RET_BACK)
                {
                    return RETURN_BACK;
                }
                else if (select_inteface_prompt_return == MINI_INPUT_RET_YES)
                {
                    *usb_selected = TRUE;
                    interface_id = 0;
                }
                else if (select_inteface_prompt_return == MINI_INPUT_RET_NO)
                {
                    *usb_selected = FALSE;
                    interface_id = 1;
                }
                else
                {
                    return RETURN_OK;
                }
            }
            else if (logic_bluetooth_get_state() == BT_STATE_CONNECTED)
            {
                /* Magic numbers FTW */
                *usb_selected = FALSE;
                interface_id = 1;
            }
            else
            {
                /* USB connected */
                *usb_selected = TRUE;
                interface_id = 0;
            }
            
            /* Move to next state */
            state_machine++;
        }
        else if (state_machine == 1)
        {
            /* Check for presence of an actual login */
            if (temp_cnode.login[0] == 0)
            {
                state_machine++;
            } 
            else
            {
                /* Ask for login confirmation */
                custom_fs_get_string_from_file(TYPE_LOGIN_TEXT_ID, &two_line_prompt_2, TRUE);
                conf_text_2_lines.lines[1] = two_line_prompt_2;
                mini_input_yes_no_ret_te prompt_return = gui_prompts_ask_for_confirmation(2, &conf_text_2_lines, FALSE, TRUE);

                /* Approved, back, card removed... */
                if (prompt_return == MINI_INPUT_RET_CARD_REMOVED)
                {
                    return RETURN_OK;
                }
                else if (prompt_return == MINI_INPUT_RET_BACK)
                {
                    /* Check for multiple interfaces connected */
                    if ((logic_bluetooth_get_state() == BT_STATE_CONNECTED) && (logic_aux_mcu_is_usb_enumerated() != FALSE))
                    {
                        state_machine = 0;
                    }
                    else
                    {
                        /* No multiple interfaces connected, leave function */
                        return RETURN_BACK;
                    }
                }
                else
                {
                    if (prompt_return == MINI_INPUT_RET_YES)
                    {
                        aux_mcu_message_t* typing_message_to_be_sent;
                        comms_aux_mcu_get_empty_packet_ready_to_be_sent(&typing_message_to_be_sent, AUX_MCU_MSG_TYPE_KEYBOARD_TYPE);
                        typing_message_to_be_sent->payload_length1 = MEMBER_SIZE(keyboard_type_message_t, interface_identifier) + MEMBER_SIZE(keyboard_type_message_t, delay_between_types) + (utils_strlen(temp_cnode.login) + 1 + 1)*sizeof(cust_char_t);
                        ret_type_te string_to_key_points_transform_success = custom_fs_get_keyboard_symbols_for_unicode_string(temp_cnode.login, typing_message_to_be_sent->keyboard_type_message.keyboard_symbols, *usb_selected);
                        if ((temp_cnode.keyAfterLogin == 0xFFFF) || ((logic_user_get_user_security_flags() & USER_SEC_FLG_ADVANCED_MENU) == 0))
                        {
                            /* Use default device key press: login is 0 terminated by read function, _Static_asserts guarantees enough space, message is initialized at 0s */
                            typing_message_to_be_sent->keyboard_type_message.keyboard_symbols[utils_strlen(temp_cnode.login)] = custom_fs_settings_get_device_setting(SETTINGS_CHAR_AFTER_LOGIN_PRESS);
                        } 
                        else
                        {
                            typing_message_to_be_sent->keyboard_type_message.keyboard_symbols[utils_strlen(temp_cnode.login)] = temp_cnode.keyAfterLogin;
                        }
                        custom_fs_get_keyboard_symbols_for_unicode_string(&typing_message_to_be_sent->keyboard_type_message.keyboard_symbols[utils_strlen(temp_cnode.login)], &typing_message_to_be_sent->keyboard_type_message.keyboard_symbols[utils_strlen(temp_cnode.login)], *usb_selected);
                        typing_message_to_be_sent->keyboard_type_message.delay_between_types = custom_fs_settings_get_device_setting(SETTINGS_DELAY_BETWEEN_PRESSES);
                        typing_message_to_be_sent->keyboard_type_message.interface_identifier = interface_id;
                        comms_aux_mcu_send_message(TRUE);
                        
                        /* Wait for typing status */
                        while(comms_aux_mcu_active_wait(&temp_rx_message, FALSE, AUX_MCU_MSG_TYPE_KEYBOARD_TYPE, FALSE, -1) != RETURN_OK){}
                        could_type_all_symbols = (BOOL)temp_rx_message->payload_as_uint16[0];
                            
                        /* Rearm DMA RX */
                        comms_aux_arm_rx_and_clear_no_comms();
                        
                        /* Display warning if some chars were missing */
                        if ((string_to_key_points_transform_success != RETURN_OK) || (could_type_all_symbols == FALSE))
                        {
                            gui_prompts_display_information_on_screen_and_wait(COULDNT_TYPE_CHARS_TEXT_ID, DISP_MSG_WARNING, FALSE);
                        }
                        
                        /* Set bool */
                        login_or_password_typed = TRUE;
                    }

                    /* Move on */
                    state_machine++;
                }
            }            
        } 
        else if (state_machine == 2)
        {
            /* Ask for password confirmation */
            custom_fs_get_string_from_file(TYPE_PASSWORD_TEXT_ID, &two_line_prompt_2, TRUE);
            conf_text_2_lines.lines[1] = two_line_prompt_2;
            mini_input_yes_no_ret_te prompt_return = gui_prompts_ask_for_confirmation(2, &conf_text_2_lines, FALSE, TRUE);

            /* Approved, back, card removed... */
            if (prompt_return == MINI_INPUT_RET_CARD_REMOVED)
            {
                return RETURN_OK;
            }
            else if (prompt_return == MINI_INPUT_RET_BACK)
            {
                /* Check for no login */
                if (temp_cnode.login[0] == 0)
                {
                    /* Check for multiple interfaces connected */
                    if ((logic_bluetooth_get_state() == BT_STATE_CONNECTED) && (logic_aux_mcu_is_usb_enumerated() != FALSE))
                    {
                        state_machine = 0;
                    }
                    else
                    {
                        /* No login, no multiple interfaces connected, leave function */
                        return RETURN_BACK;                        
                    }                 
                }
                else
                {
                    state_machine--;
                }
            }
            else
            {
                if (prompt_return == MINI_INPUT_RET_YES)
                {
                    BOOL prev_gen_credential_flag = FALSE;
                    
                    /* Check for previous generation password */
                    if ((temp_cnode.flags & NODEMGMT_PREVGEN_BIT_BITMASK) != 0)
                    {
                        prev_gen_credential_flag = TRUE;
                    }
                    
                    /* Decrypt password. The field just after it is 0 */
                    logic_encryption_ctr_decrypt((uint8_t*)temp_cnode.password, temp_cnode.ctr, MEMBER_SIZE(child_cred_node_t, password), prev_gen_credential_flag);
                    
                    /* If old generation password, convert it to unicode */
                    if (prev_gen_credential_flag != FALSE)
                    {
                        _Static_assert(MEMBER_SIZE(child_cred_node_t, password) >= NODEMGMT_OLD_GEN_ASCII_PWD_LENGTH*2 + 2, "Backward compatibility problem");
                        utils_ascii_to_unicode((uint8_t*)temp_cnode.password, NODEMGMT_OLD_GEN_ASCII_PWD_LENGTH);
                    }
                    
                    /* Prepare packet to be sent */
                    aux_mcu_message_t* typing_message_to_be_sent;
                    comms_aux_mcu_get_empty_packet_ready_to_be_sent(&typing_message_to_be_sent, AUX_MCU_MSG_TYPE_KEYBOARD_TYPE);
                    typing_message_to_be_sent->payload_length1 = MEMBER_SIZE(keyboard_type_message_t, interface_identifier) + MEMBER_SIZE(keyboard_type_message_t, delay_between_types) + (utils_strlen(temp_cnode.cust_char_password) + 1 + 1)*sizeof(cust_char_t);
                    ret_type_te string_to_key_points_transform_success = custom_fs_get_keyboard_symbols_for_unicode_string(temp_cnode.cust_char_password, typing_message_to_be_sent->keyboard_type_message.keyboard_symbols, *usb_selected);
                    if (temp_cnode.keyAfterPassword == 0xFFFF)
                    {
                        /* Use default device key press: password is 0 terminated by read function, _Static_asserts guarantees enough space, message is initialized at 0s */
                        typing_message_to_be_sent->keyboard_type_message.keyboard_symbols[utils_strlen(temp_cnode.cust_char_password)] = custom_fs_settings_get_device_setting(SETTINGS_CHAR_AFTER_PASS_PRESS);
                    }
                    else
                    {
                        typing_message_to_be_sent->keyboard_type_message.keyboard_symbols[utils_strlen(temp_cnode.cust_char_password)] = temp_cnode.keyAfterPassword;
                    }
                    custom_fs_get_keyboard_symbols_for_unicode_string(&typing_message_to_be_sent->keyboard_type_message.keyboard_symbols[utils_strlen(temp_cnode.cust_char_password)], &typing_message_to_be_sent->keyboard_type_message.keyboard_symbols[utils_strlen(temp_cnode.cust_char_password)], *usb_selected);
                    typing_message_to_be_sent->keyboard_type_message.delay_between_types = custom_fs_settings_get_device_setting(SETTINGS_DELAY_BETWEEN_PRESSES);
                    typing_message_to_be_sent->keyboard_type_message.interface_identifier = interface_id;
                    comms_aux_mcu_send_message(TRUE);
                    
                    /* Message is sent, clear everything */
                    memset(typing_message_to_be_sent, 0, sizeof(aux_mcu_message_t));
                    memset(&temp_cnode, 0, sizeof(temp_cnode));
                    
                    /* Wait for typing status */
                    while(comms_aux_mcu_active_wait(&temp_rx_message, FALSE, AUX_MCU_MSG_TYPE_KEYBOARD_TYPE, FALSE, -1) != RETURN_OK){}
                    could_type_all_symbols = (BOOL)temp_rx_message->payload_as_uint16[0];
                    
                    /* Rearm DMA RX */
                    comms_aux_arm_rx_and_clear_no_comms();
                    
                    /* Display warning if some chars were missing */
                    if ((string_to_key_points_transform_success != RETURN_OK) || (could_type_all_symbols == FALSE))
                    {
                        gui_prompts_display_information_on_screen_and_wait(COULDNT_TYPE_CHARS_TEXT_ID, DISP_MSG_WARNING, FALSE);
                    }
                        
                    /* Set bool */
                    login_or_password_typed = TRUE;
                    
                    /* Move on */
                    return RETURN_OK;
                }
                else
                {
                    if (login_or_password_typed == FALSE)
                    {
                        return RETURN_NOK;
                    }
                    else
                    {
                        return RETURN_OK;
                    }
                }
            }
        }
    }

    return RETURN_OK;
}