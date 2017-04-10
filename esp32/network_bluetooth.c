/*
 * This file is part of the MicroPython project, http://micropython.org/
 *
 * The MIT License (MIT)
 *
 * Copyright (c) 2017 "Eric Poulsen" <eric@zyxod.com>
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */

#include <stdio.h>
#include <stdint.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "py/runtime.h"
#include "py/stream.h"
#include "py/mphal.h"
#include "py/objstr.h"
#include "modmachine.h"

#include "bt.h"
#include "esp_gap_ble_api.h"

#define HCI_GRP_HOST_CONT_BASEBAND_CMDS    0x03
#define HCI_GRP_BLE_CMDS                   0x08

#define H4_TYPE_COMMAND 0x01
#define H4_TYPE_ACL     0x02
#define H4_TYPE_SCO     0x03
#define H4_TYPE_EVENT   0x04

#define STRINGIFY(x) #x
#define TOSTRING(x) STRINGIFY(x)

#define MAKE_OPCODE(OGF, OCF) (((OGF) << 10) | (OCF))
#define MAKE_OPCODE_BYTES(OGF, OCF) { (MAKE_OPCODE(OGF, OCF) & 0xff), (MAKE_OPCODE(OGF, OCF) >> 8) }

#define BD_ADDR_LEN     (6)                     /* Device address length */
typedef uint8_t bd_addr_t[BD_ADDR_LEN];         /* Device address */

#define UINT16_TO_STREAM(p, u16) {*(p)++ = (uint8_t)(u16); *(p)++ = (uint8_t)((u16) >> 8);}
#define UINT8_TO_STREAM(p, u8)   {*(p)++ = (uint8_t)(u8);}
#define BDADDR_TO_STREAM(p, a)   {int ijk; for (ijk = 0; ijk < BD_ADDR_LEN;  ijk++) *(p)++ = (uint8_t) a[BD_ADDR_LEN - 1 - ijk];}
#define ARRAY_TO_STREAM(p, a, len) {int ijk; for (ijk = 0; ijk < len;        ijk++) *(p)++ = (uint8_t) a[ijk];}

const mp_obj_type_t network_bluetooth_type;

typedef struct {
    mp_obj_base_t base;
    enum {
        NETWORK_BLUETOOTH_STATE_DEINIT,
        NETWORK_BLUETOOTH_STATE_INIT
    } state;
    esp_ble_adv_params_t params;
    esp_ble_adv_data_t   data;

} network_bluetooth_obj_t;

STATIC network_bluetooth_obj_t network_bluetooth_singleton = { 
    .base = { &network_bluetooth_type },
    .params = {
        .adv_int_min = 1280 * 1.6,
        .adv_int_max = 1280 * 1.6,
        .adv_type = ADV_TYPE_IND,
        .own_addr_type = BLE_ADDR_TYPE_PUBLIC,
        .peer_addr = { 0,0,0,0,0,0 },
        .peer_addr_type = BLE_ADDR_TYPE_PUBLIC,
        .channel_map = ADV_CHNL_ALL, 
        .adv_filter_policy = ADV_FILTER_ALLOW_SCAN_ANY_CON_ANY,
    },
    .data = {
        .set_scan_rsp = false,
        .include_name = false,
        .include_txpower = false,
        .min_interval = 1280 * 1.6,
        .max_interval = 1280 * 1.6,
        .appearance = 0,
        .p_manufacturer_data = NULL,
        .manufacturer_len = 0,
        .p_service_data = NULL,
        .service_data_len = 0,
        .p_service_uuid = 0,
        .flag = 0
    },
}; 

typedef struct {
    union {
        uint8_t preamble[3];
        struct {
            uint8_t opcode[2];
            uint8_t param_size;
        };
    };
} hci_cmd_def_t;

hci_cmd_def_t hci_commands[4] = {
    {
        .opcode         = MAKE_OPCODE_BYTES(HCI_GRP_HOST_CONT_BASEBAND_CMDS, 0x03), // HCI_RESET
        .param_size     = 0x00,
    },
    {
        .opcode         = MAKE_OPCODE_BYTES(HCI_GRP_BLE_CMDS, 0x0A), // HCI_BLE_WRITE_ADV_ENABLE
        .param_size     = 0x01,
    },
    {
        .opcode         = MAKE_OPCODE_BYTES(HCI_GRP_BLE_CMDS, 0x06), // HCI_BLE_WRITE_ADV_PARAMS
        .param_size     = 0x0f,
    },
    {
        .opcode         = MAKE_OPCODE_BYTES(HCI_GRP_BLE_CMDS, 0x08), // HCI_BLE_WRITE_ADV_DATA
        .param_size     = 0x1f,
    },
};

enum {
    HCI_CMD_RESET                   = 0,
    HCI_CMD_BLE_WRITE_ADV_ENABLE    = 1,
    HCI_CMD_BLE_WRITE_ADV_PARAMS    = 2,
    HCI_CMD_BLE_WRITE_ADV_DATA      = 3
} hci_cmd_t;

#define NETWORK_BLUETOOTH_DEBUG_PRINTF(args...) printf(args)
#define CREATE_HCI_HOST_COMMAND(cmd)\
    size_t param_size = hci_commands[(cmd)].param_size;\
    size_t buf_size = 1 + sizeof(hci_cmd_def_t) + param_size;\
    uint8_t buf[buf_size];\
    uint8_t *param = buf + buf_size - param_size;\
    memset(buf, 0, buf_size);\
    buf[0] = H4_TYPE_COMMAND;\
    memcpy(buf + 1, &hci_commands[(cmd)], sizeof(hci_cmd_def_t)); 

STATIC void dumpBuf(const uint8_t *buf, size_t len) {
    while(len--) 
        printf("%02X ", *buf++);
    printf("\n");
}

STATIC void network_bluetooth_send_data(uint8_t *buf, size_t buf_size) {
    NETWORK_BLUETOOTH_DEBUG_PRINTF("Entering network_bluetooth_send_data\n");
    int tries = 3;
    bool ready;

    // FIXME: A somewhat naïve approach; look into using esp_vhci_host_callback
    while(((ready = esp_vhci_host_check_send_available()) == false) && tries--) {
        NETWORK_BLUETOOTH_DEBUG_PRINTF("network_bluetooth_send_data: waiting for host to be ready\n");
        vTaskDelay((10 / portTICK_PERIOD_MS) || 1);
    }
    //FIXME
    printf("Sending: ");
    dumpBuf(buf, buf_size);

    esp_vhci_host_send_packet(buf, buf_size);
}

static void network_bluetooth_send_hci_reset() {
    CREATE_HCI_HOST_COMMAND(HCI_CMD_RESET);
    (void)param;
    network_bluetooth_send_data(buf, buf_size);
}


/******************************************************************************/
// MicroPython bindings for network_bluetooth


STATIC void network_bluetooth_print(const mp_print_t *print, mp_obj_t self_in, mp_print_kind_t kind) {
    network_bluetooth_obj_t *self = MP_OBJ_TO_PTR(self_in);
    mp_printf(print, "Bluetooth(params=())", self);
    NETWORK_BLUETOOTH_DEBUG_PRINTF(
            "Bluetooth(params = ("
            "adv_int_min = %u, "
            "adv_int_max = %u, "
            "adv_type = %u, "
            "own_addr_type = %u, "
            "peer_addr = %02X:%02X:%02X:%02X:%02X:%02X, "
            "peer_addr_type = %u, "
            "channel_map = %u, "
            "adv_filter_policy = %u"
            ")"
            ")\n"
            ,
            (unsigned int)(self->params.adv_int_min / 1.6),
            (unsigned int)(self->params.adv_int_max / 1.6),
            self->params.adv_type,
            self->params.own_addr_type,
            self->params.peer_addr[0],
            self->params.peer_addr[1],
            self->params.peer_addr[2],
            self->params.peer_addr[3],
            self->params.peer_addr[4],
            self->params.peer_addr[5],
            self->params.peer_addr_type,
            self->params.channel_map,
            self->params.adv_filter_policy
                );
}


STATIC mp_obj_t network_bluetooth_init(mp_obj_t self_in) {
    network_bluetooth_obj_t * self = (network_bluetooth_obj_t*)self_in;
    if (self->state == NETWORK_BLUETOOTH_STATE_DEINIT) {
        NETWORK_BLUETOOTH_DEBUG_PRINTF("BT is deinit, initializing\n");

        esp_bt_controller_init(); 
        esp_err_t ret = esp_bt_controller_enable(ESP_BT_MODE_BTDM);

        switch(ret) {
            case ESP_OK:
                NETWORK_BLUETOOTH_DEBUG_PRINTF("BT initialization ok\n");
                break;
            default:
                mp_raise_msg(&mp_type_OSError, "BT initialization failed");

        }
        network_bluetooth_send_hci_reset();

        self->state = NETWORK_BLUETOOTH_STATE_INIT;

    } else {
        NETWORK_BLUETOOTH_DEBUG_PRINTF("BT already initialized\n");
    }
    return mp_const_none;
}
STATIC MP_DEFINE_CONST_FUN_OBJ_1(network_bluetooth_init_obj, network_bluetooth_init);

STATIC mp_obj_t network_bluetooth_ble_settings(size_t n_args, const mp_obj_t *pos_args, mp_map_t * kw_args) {
    network_bluetooth_obj_t *self = &network_bluetooth_singleton;
    NETWORK_BLUETOOTH_DEBUG_PRINTF("Entering network_bluetooth_ble_settings(self = %p) n_args = %d\n", self, n_args);

    bool changed = false;
    enum { 
        // params
        ARG_int_min,
        ARG_int_max,
        ARG_type,
        ARG_own_addr_type,
        ARG_peer_addr,
        ARG_peer_addr_type,
        ARG_channel_map,
        ARG_filter_policy,

        // data
        ARG_adv_is_scan_rsp,
        ARG_adv_dev_name,
        ARG_adv_man_name,
        ARG_adv_inc_txpower,
        ARG_adv_int_min,
        ARG_adv_int_max,
        ARG_adv_appearance,
        ARG_adv_uuid,
        ARG_adv_flags
    };

    static const mp_arg_t allowed_args[] = {
        { MP_QSTR_int_min,              MP_ARG_KW_ONLY | MP_ARG_INT, {.u_int = -1} },
        { MP_QSTR_int_max,              MP_ARG_KW_ONLY | MP_ARG_INT, {.u_int = -1} },
        { MP_QSTR_type,                 MP_ARG_KW_ONLY | MP_ARG_INT, {.u_int = -1 }},
        { MP_QSTR_own_addr_type,        MP_ARG_KW_ONLY | MP_ARG_INT, {.u_int = -1 }},
        { MP_QSTR_peer_addr,            MP_ARG_KW_ONLY | MP_ARG_OBJ, {.u_obj = NULL }},
        { MP_QSTR_peer_addr_type,       MP_ARG_KW_ONLY | MP_ARG_INT, {.u_int = -1 }},
        { MP_QSTR_channel_map,          MP_ARG_KW_ONLY | MP_ARG_INT, {.u_int = -1 }},
        { MP_QSTR_filter_policy,        MP_ARG_KW_ONLY | MP_ARG_INT, {.u_int = -1 }},

        { MP_QSTR_adv_is_scan_rsp,      MP_ARG_KW_ONLY | MP_ARG_INT, {.u_int = -1 }},
        { MP_QSTR_adv_dev_name,         MP_ARG_KW_ONLY | MP_ARG_OBJ, {.u_obj = NULL }},
        { MP_QSTR_adv_man_name,         MP_ARG_KW_ONLY | MP_ARG_OBJ, {.u_obj = NULL }},
        { MP_QSTR_adv_inc_tx_power,     MP_ARG_KW_ONLY | MP_ARG_INT, {.u_int = -1 }},
        { MP_QSTR_adv_int_min,          MP_ARG_KW_ONLY | MP_ARG_INT, {.u_int = -1 }},
        { MP_QSTR_adv_int_max,          MP_ARG_KW_ONLY | MP_ARG_INT, {.u_int = -1 }},
        { MP_QSTR_adv_appearance,       MP_ARG_KW_ONLY | MP_ARG_INT, {.u_int = -1 }},
        { MP_QSTR_adv_uuid,             MP_ARG_KW_ONLY | MP_ARG_OBJ, {.u_obj = NULL }},
        { MP_QSTR_adv_flags,            MP_ARG_KW_ONLY | MP_ARG_INT, {.u_int = NULL }},
    };

    mp_arg_val_t args[MP_ARRAY_SIZE(allowed_args)];
    mp_arg_parse_all(0, pos_args, kw_args, MP_ARRAY_SIZE(allowed_args), allowed_args, args);

    mp_buffer_info_t peer_addr_buf = { 0 };
    mp_buffer_info_t adv_man_name_buf = { 0 };
    mp_buffer_info_t adv_dev_name_buf = { 0 };


    // pre-check complex types
    if (args[ARG_peer_addr].u_obj != MP_OBJ_NULL) {
        if (mp_obj_get_type(args[ARG_peer_addr].u_obj) != &mp_type_bytearray) {
            goto network_bluetooth_bad_byte_array;
        }

        mp_get_buffer(args[ARG_peer_addr].u_obj, &peer_addr_buf, MP_BUFFER_READ);
        if (peer_addr_buf.len != ESP_BD_ADDR_LEN) {
            goto network_bluetooth_bad_byte_array;
        }
    }

    if (args[ARG_adv_man_name].u_obj != MP_OBJ_NULL) {
        if (mp_obj_get_type(args[ARG_adv_man_name].u_obj) == mp_const_none) {
            self->data.manufacturer_len = 0;
            if (self->data.p_manufacturer_data != NULL) {
                m_free(self->data.p_manufacturer_data);
                self->data.p_manufacturer_data = NULL;
            }
        } else if (!MP_OBJ_IS_STR_OR_BYTES(args[ARG_adv_man_name].u_obj)) {
            mp_raise_ValueError("adv_man_name must be type str or bytes");
        } 
        mp_obj_str_get_buffer(args[ARG_peer_addr].u_obj, &adv_man_name_buf, MP_BUFFER_READ);
    }

    if (args[ARG_adv_dev_name].u_obj != MP_OBJ_NULL) {
        if (mp_obj_get_type(args[ARG_adv_dev_name].u_obj) == mp_const_none) {
            esp_ble_gap_set_device_name("");
            self->data.include_name = false;
        } else if (!MP_OBJ_IS_STR_OR_BYTES(args[ARG_adv_dev_name].u_obj)) {
            mp_raise_ValueError("adv_dev_name must be type str or bytes");
        }
        mp_obj_str_get_buffer(args[ARG_peer_addr].u_obj, &adv_dev_name_buf, MP_BUFFER_READ);
    }


    // update esp_ble_adv_params_t 
    
    if (args[ARG_int_min].u_int != -1) {
        self->params.adv_int_min = args[ARG_int_min].u_int * 1.6; // 0.625 msec per count
        changed = true;
    }
    if (args[ARG_int_max].u_int != -1) {
        self->params.adv_int_max = args[ARG_int_max].u_int * 1.6;
        changed = true;
    }
    if (args[ARG_type].u_int != -1) {
        self->params.adv_type = args[ARG_type].u_int;
        changed = true;
    }
    if (args[ARG_own_addr_type].u_int != -1) {
        self->params.own_addr_type = args[ARG_own_addr_type].u_int;
        changed = true;
    }

    if (peer_addr_buf.buf != NULL) {
        memcpy(self->params.peer_addr, peer_addr_buf.buf, ESP_BD_ADDR_LEN);
        changed = true;
    }

    if (args[ARG_peer_addr_type].u_int != -1) {
        self->params.peer_addr_type = args[ARG_peer_addr_type].u_int;
        changed = true;
    }
    if (args[ARG_channel_map].u_int != -1) {
        self->params.channel_map = args[ARG_channel_map].u_int;
        changed = true;
    }
    if (args[ARG_filter_policy].u_int != -1) {
        self->params.adv_filter_policy = args[ARG_filter_policy].u_int;
        changed = true;
    }


    // update esp_ble_adv_data_t 
    //
    
    if (args[ARG_adv_is_scan_rsp].u_int != -1) {
        self->data.set_scan_rsp = mp

    }

    if (adv_dev_name_buf.buf != NULL) {
        esp_ble_gap_set_device_name(adv_dev_name_buf.buf);
        self->data.include_name = adv_dev_name_buf.len > 0;
        changed = true;
    }

    if (adv_man_name_buf.buf != NULL) {

        self->data.manufacturer_len = 0;
        if (self->data.p_manufacturer_data != NULL) {
            m_free(self->data.p_manufacturer_data);
            self->data.p_manufacturer_data = NULL;
        }

        if (buffer.len > 0) {
            self->data.p_manufacturer_data = m_malloc(adv_man_name_buf.len);
            memcpy(self->data.p_manufacturer_data, buffer.buf, buffer.len);
            self->data.include_name = adv_man_name_buf.len > 0;
            self->data.manufacturer_len = 0;
        }
        changed = true;
    }

    return mp_const_none;

network_bluetooth_bad_byte_array:
    mp_raise_ValueError("peer_addr must be bytearray(" TOSTRING(ESP_BD_ADDR_LEN) ")");
}
STATIC MP_DEFINE_CONST_FUN_OBJ_KW(network_bluetooth_ble_settings_obj, 1, network_bluetooth_ble_settings);

STATIC mp_obj_t network_bluetooth_make_new(const mp_obj_type_t *type_in, size_t n_args, size_t n_kw, const mp_obj_t *all_args) {

    network_bluetooth_obj_t *self = &network_bluetooth_singleton;
    NETWORK_BLUETOOTH_DEBUG_PRINTF("Entering network_bluetooth_make_new, self = %p, n_args = %d, n_kw = %d\n", self, n_args, n_kw );
    if (n_args != 0 || n_kw != 0) {
        mp_raise_TypeError("Constructor takes no arguments");
    }

    network_bluetooth_init(self);
    return MP_OBJ_FROM_PTR(self);
}

STATIC mp_obj_t network_bluetooth_deinit(mp_obj_t self_in) {
    NETWORK_BLUETOOTH_DEBUG_PRINTF("Entering network_bluetooth_deinit\n");
    // FIXME
    return mp_const_none;
}
STATIC MP_DEFINE_CONST_FUN_OBJ_1(network_bluetooth_deinit_obj, network_bluetooth_deinit);


STATIC const mp_rom_map_elem_t network_bluetooth_locals_dict_table[] = {
    // instance methods
    { MP_ROM_QSTR(MP_QSTR_ble_settings), MP_ROM_PTR(&network_bluetooth_ble_settings_obj) },
    { MP_ROM_QSTR(MP_QSTR_init), MP_ROM_PTR(&network_bluetooth_init_obj) },
    { MP_ROM_QSTR(MP_QSTR_deinit), MP_ROM_PTR(&network_bluetooth_deinit_obj) },

    // class constants

    // esp_ble_adv_type_t
    { MP_ROM_QSTR(MP_QSTR_ADV_TYPE_IND),             MP_ROM_INT(ADV_TYPE_IND) },
    { MP_ROM_QSTR(MP_QSTR_ADV_TYPE_DIRECT_IND_HIGH), MP_ROM_INT(ADV_TYPE_DIRECT_IND_HIGH) },
    { MP_ROM_QSTR(MP_QSTR_ADV_TYPE_SCAN_IND),        MP_ROM_INT(ADV_TYPE_SCAN_IND) },
    { MP_ROM_QSTR(MP_QSTR_ADV_TYPE_NONCONN_IND),     MP_ROM_INT(ADV_TYPE_NONCONN_IND) },
    { MP_ROM_QSTR(MP_QSTR_ADV_TYPE_DIRECT_IND_LOW),  MP_ROM_INT(ADV_TYPE_DIRECT_IND_LOW) },

    // esp_ble_addr_type_t
    { MP_ROM_QSTR(MP_QSTR_BLE_ADDR_TYPE_PUBLIC),     MP_ROM_INT(BLE_ADDR_TYPE_PUBLIC) },
    { MP_ROM_QSTR(MP_QSTR_BLE_ADDR_TYPE_RANDOM),     MP_ROM_INT(BLE_ADDR_TYPE_RANDOM) },
    { MP_ROM_QSTR(MP_QSTR_BLE_ADDR_TYPE_RPA_PUBLIC), MP_ROM_INT(BLE_ADDR_TYPE_RPA_PUBLIC) },
    { MP_ROM_QSTR(MP_QSTR_BLE_ADDR_TYPE_RPA_RANDOM), MP_ROM_INT(BLE_ADDR_TYPE_RPA_RANDOM) },

    // esp_ble_adv_channel_t
    { MP_ROM_QSTR(MP_QSTR_ADV_CHNL_37),              MP_ROM_INT(ADV_CHNL_37) },
    { MP_ROM_QSTR(MP_QSTR_ADV_CHNL_38),              MP_ROM_INT(ADV_CHNL_38) },
    { MP_ROM_QSTR(MP_QSTR_ADV_CHNL_39),              MP_ROM_INT(ADV_CHNL_39) },
    { MP_ROM_QSTR(MP_QSTR_ADV_CHNL_ALL),             MP_ROM_INT(ADV_CHNL_ALL) },

    // esp_ble_adv_filter_t
    { MP_ROM_QSTR(MP_QSTR_ADV_FILTER_ALLOW_SCAN_ANY_CON_ANY),
        MP_ROM_INT(ADV_FILTER_ALLOW_SCAN_ANY_CON_ANY) },
    { MP_ROM_QSTR(MP_QSTR_ADV_FILTER_ALLOW_SCAN_WLST_CON_ANY),
        MP_ROM_INT(ADV_FILTER_ALLOW_SCAN_WLST_CON_ANY) },
    { MP_ROM_QSTR(MP_QSTR_ADV_FILTER_ALLOW_SCAN_ANY_CON_WLST),
        MP_ROM_INT(ADV_FILTER_ALLOW_SCAN_ANY_CON_WLST) },
    { MP_ROM_QSTR(MP_QSTR_ADV_FILTER_ALLOW_SCAN_WLST_CON_WLST),
        MP_ROM_INT(ADV_FILTER_ALLOW_SCAN_WLST_CON_WLST) },
};

STATIC MP_DEFINE_CONST_DICT(network_bluetooth_locals_dict, network_bluetooth_locals_dict_table);

const mp_obj_type_t network_bluetooth_type = {
    { &mp_type_type },
    .name = MP_QSTR_Bluetooth,
    .print = network_bluetooth_print,
    .make_new = network_bluetooth_make_new,
    .locals_dict = (mp_obj_dict_t*)&network_bluetooth_locals_dict,
};
