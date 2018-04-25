/*****************************************************************************
	Copyright(c) 2013 FCI Inc. All Rights Reserved

	File name : fc8300_isr.c

	Description : source of interrupt service routine

	This program is free software; you can redistribute it and/or modify
	it under the terms of the GNU General Public License as published by
	the Free Software Foundation; either version 2 of the License, or
	(at your option) any later version.

	This program is distributed in the hope that it will be useful,
	but WITHOUT ANY WARRANTY; without even the implied warranty of
	MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
	GNU General Public License for more details.

	You should have received a copy of the GNU General Public License
	along with this program; if not, write to the Free Software
	Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA 02111-1307 USA

	History :
	----------------------------------------------------------------------
*******************************************************************************/
#include <linux/kernel.h>
#include <linux/slab.h>
#include <linux/module.h>

#include "fci_types.h"
#include "fc8300_regs.h"
#include "fci_hal.h"

s32 (*fc8300_ac_callback)
	(ulong userdata, u8 bufid, u8 *data, s32 length) = NULL;
s32 (*fc8300_ts_callback)
	(ulong userdata, u8 bufid, u8 *data, s32 length) = NULL;

ulong fc8300_ac_user_data;
ulong fc8300_ts_user_data;

#ifndef BBM_I2C_TSIF
static u8 ts_buffer[TS0_BUF_LENGTH];
//static u8 ac_buffer[AC_B_BUF_LENGTH];

static void fc8300_data(HANDLE handle, DEVICEID devid, u8 buf_int_status)
{
	u32 size = 0;
//	s32 i;

	if (buf_int_status == 1) {
#if 0
		bbm_word_read(handle, devid,
					BBM_BUF_TS0_THR,
					(u16 *) &size);

		if ((size == 0)||(size > TS0_BUF_LENGTH))
			continue;
		size = (size + 1) << 1;
#endif
		size = TS0_BUF_LENGTH;
		bbm_data(handle, devid,
				BBM_TS0_DATA, &ts_buffer[0], size);

		if (fc8300_ts_callback)
			(*fc8300_ts_callback)(fc8300_ts_user_data,
					0, &ts_buffer[0], size);
	}
#if 0
	for (i = 0; (i < 4) && (buf_int_status & 0x0f); i++) {
		if (buf_int_status & (1 << i)) {
			bbm_word_read(handle, devid,
					BBM_BUF_TS0_THR + (i << 1),
					(u16 *) &size);

			if ((size == 0)||(size > TS0_BUF_LENGTH))
				continue;

			size = (size + 1) << 1;

			bbm_data(handle, devid,
					BBM_TS0_DATA + i, &ts_buffer[0], size);

			if (fc8300_ts_callback)
				(*fc8300_ts_callback)(fc8300_ts_user_data,
						i, &ts_buffer[0], size);
		}
	}

	for (i = 4; i < 8 && (buf_int_status & 0xf0); i++) {
		if (buf_int_status & (1 << i)) {
			bbm_word_read(handle, devid,
					BBM_BUF_TS0_THR + (i << 1),
					(u16 *) &size);

			if ((size == 0)||(size > AC_B_BUF_LENGTH))
				continue;

			size = (size + 1) << 1;

			bbm_data(handle, devid,
					BBM_TS0_DATA + i, &ac_buffer[0], size);

			if (fc8300_ac_callback)
				(*fc8300_ac_callback)(fc8300_ac_user_data,
						i, &ac_buffer[0], size);
		}
	}
#endif
}
#endif /* #ifndef BBM_I2C_TSIF */

#ifdef BBM_AUX_INT
static void fc8300_aux_int(HANDLE handle, DEVICEID devid, u8 aux_int_status)
{
	if (aux_int_status & AUX_INT_TMCC_INT_SRC)
		;

	if (aux_int_status & AUX_INT_TMCC_INDTPS_SRC)
		;

	if (aux_int_status & AUX_INT_AC_PREFRM_SRC)
		;

	if (aux_int_status & AUX_INT_AC_EWISTAFLAG_SRC)
		;

	if (aux_int_status & AUX_INT_SYNC_RELATED_INT) {
		u8 sync = 0;
		bbm_byte_read(handle, DIV_MASTER, BBM_SYS_MD_INT_CLR, &sync);

		if (sync) {
			bbm_byte_write(handle, DIV_MASTER, BBM_SYS_MD_INT_CLR,
									sync);

			if (sync & SYS_MD_NO_OFDM_DETECT)
				;

			if (sync & SYS_MD_RESYNC_OCCUR)
				;

			if (sync & SYS_MD_TMCC_LOCK)
				;

			if (sync & SYS_MD_A_LAYER_BER_UPDATE)
				;

			if (sync & SYS_MD_B_LAYER_BER_UPDATE)
				;

			if (sync & SYS_MD_C_LAYER_BER_UPDATE)
				;

			if (sync & SYS_MD_BER_UPDATE)
				;
		}
	}

	if (aux_int_status & AUX_INT_GPIO_INT_CLEAR)
		;

	if (aux_int_status & AUX_INT_FEC_RELATED_INT) {
		u8 fec = 0;
		bbm_byte_read(handle, DIV_MASTER, BBM_FEC_INT_CLR, &fec);

		if (fec) {
			bbm_byte_write(handle, DIV_MASTER, BBM_FEC_INT_CLR,
								fec);

			if (fec & FEC_INT_IRQ_A_TS_ERROR)
				;

			if (fec & FEC_INT_IRQ_B_TS_ERROR)
				;

			if (fec & FEC_INT_IRQ_C_TS_ERROR)
				;
		}
	}

	if (aux_int_status & AUX_INT_AUTO_SWITCH) {
		u8 auto_switch = 0;
		bbm_byte_read(handle, DIV_MASTER, BBM_OSS_MNT, &auto_switch);

		if (auto_switch & AUTO_SWITCH_1_SEG) /* 1-SEG */
			;
		else  /* 12-SEG */
			;
	}
}
#endif

void fc8300_isr(HANDLE handle)
{
#ifdef BBM_AUX_INT
	u8 aux_int_status = 0;
#endif

#ifndef BBM_I2C_TSIF
	u8 buf_int_status = 0;
//	pr_info("%s %d jgk +++\n", __func__, __LINE__);
	bbm_byte_read(handle, DIV_MASTER, BBM_BUF_STATUS_CLEAR,
					&buf_int_status);
	if (buf_int_status) {
		bbm_byte_write(handle, DIV_MASTER,
				BBM_BUF_STATUS_CLEAR, buf_int_status);

		fc8300_data(handle, DIV_MASTER, buf_int_status);
	}
	buf_int_status = 0;
	bbm_byte_read(handle, DIV_MASTER, BBM_BUF_STATUS_CLEAR,
					&buf_int_status);
	if (buf_int_status) {
		bbm_byte_write(handle, DIV_MASTER,
				BBM_BUF_STATUS_CLEAR, buf_int_status);

		fc8300_data(handle, DIV_MASTER, buf_int_status);
	}
#endif

#ifdef BBM_AUX_INT
	bbm_byte_read(handle, DIV_MASTER, BBM_AUX_STATUS_CLEAR,
					&aux_int_status);

	if (aux_int_status) {
		bbm_byte_write(handle, DIV_MASTER,
				BBM_AUX_STATUS_CLEAR, aux_int_status);

		fc8300_aux_int(handle, DIV_MASTER, aux_int_status);
	}
#endif
}

