/* linux/drivers/video/mdnie.c
 *
 * Register interface file for Samsung mDNIe driver
 *
*/

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/errno.h>
#include <linux/mutex.h>
#include <linux/mm.h>
#include <linux/device.h>
#include <linux/backlight.h>
#include <linux/platform_device.h>
#include <linux/delay.h>
#include <linux/lcd.h>
#include <linux/fb.h>
#include <linux/pm_runtime.h>
#include "../decon_notify.h"

#include "mdnie.h"
#ifdef CONFIG_DISPLAY_USE_INFO
#include "dpui.h"
#endif

#define MDNIE_SYSFS_PREFIX		"/sdcard/mdnie/"

#define IS_DMB(idx)					(idx == DMB_NORMAL_MODE)
#define IS_SCENARIO(idx)			(idx < SCENARIO_MAX && !(idx > VIDEO_NORMAL_MODE && idx < CAMERA_MODE))
#define IS_ACCESSIBILITY(idx)		(idx && idx < ACCESSIBILITY_MAX)
#define IS_HBM(idx)					(idx && idx < HBM_MAX)
#define IS_HMT(idx)					(idx && idx < HMT_MDNIE_MAX)
#define IS_LIGHT_NOTIFICATION(idx)	(idx && idx < LIGHT_NOTIFICATION_MAX)

#define SCENARIO_IS_VALID(idx)	(IS_DMB(idx) || IS_SCENARIO(idx))
#define IS_NIGHT_MODE(idx)		(idx == NIGHT_MODE_ON)

/* Split 16 bit as 8bit x 2 */
#define GET_MSB_8BIT(x)		((x >> 8) & (BIT(8) - 1))
#define GET_LSB_8BIT(x)		((x >> 0) & (BIT(8) - 1))

static struct class *mdnie_class;

/* Do not call mdnie write directly */
static int mdnie_write(struct mdnie_info *mdnie, struct mdnie_table *table, unsigned int num)
{
	int ret = 0;

	if (mdnie->enable)
		ret = mdnie->ops.write(mdnie->data, table->seq, num);

	return ret;
}

static int mdnie_write_table(struct mdnie_info *mdnie, struct mdnie_table *table)
{
	int i, ret = 0;
	struct mdnie_table *buf = NULL;

	for (i = 0; table->seq[i].len; i++) {
		if (IS_ERR_OR_NULL(table->seq[i].cmd)) {
			dev_info(mdnie->dev, "mdnie sequence %s %dth is null\n", table->name, i);
			return -EPERM;
		}
	}

	mutex_lock(&mdnie->dev_lock);

	buf = table;

	ret = mdnie_write(mdnie, buf, i);

	mutex_unlock(&mdnie->dev_lock);

	return ret;
}

static struct mdnie_table *mdnie_find_table(struct mdnie_info *mdnie)
{
	struct mdnie_table *table = NULL;
	struct mdnie_trans_info *trans_info = mdnie->tune->trans_info;

	mutex_lock(&mdnie->lock);

	if (IS_LIGHT_NOTIFICATION(mdnie->light_notification)) {
		table = mdnie->tune->light_notification_table ? &mdnie->tune->light_notification_table[mdnie->light_notification] : NULL;
		goto exit;
	}else if (IS_ACCESSIBILITY(mdnie->accessibility)) {
		table = mdnie->tune->accessibility_table ? &mdnie->tune->accessibility_table[mdnie->accessibility] : NULL;
		goto exit;
	} else if (IS_HMT(mdnie->hmt_mode)) {
		table = mdnie->tune->hmt_table ? &mdnie->tune->hmt_table[mdnie->hmt_mode] : NULL;
		goto exit;
	} else if (IS_NIGHT_MODE(mdnie->night_mode)) {
		table = mdnie->tune->night_table ? &mdnie->tune->night_table[mdnie->night_mode] : NULL;
		goto exit;
	} else if (IS_HBM(mdnie->hbm)) {
		table = mdnie->tune->hbm_table ? &mdnie->tune->hbm_table[mdnie->hbm] : NULL;
		goto exit;
	} else if (IS_DMB(mdnie->scenario)) {
		table = mdnie->tune->dmb_table ? &mdnie->tune->dmb_table[mdnie->mode] : NULL;
		goto exit;
	} else if (IS_SCENARIO(mdnie->scenario)) {
		table = mdnie->tune->main_table ? &mdnie->tune->main_table[mdnie->scenario][mdnie->mode] : NULL;
		goto exit;
	}

exit:
	if (trans_info->enable && mdnie->disable_trans_dimming && (table != NULL)) {
		dev_info(mdnie->dev, "%s: disable_trans_dimming=%d\n", __func__, mdnie->disable_trans_dimming);
		memcpy(&(mdnie->table_buffer), table, sizeof(struct mdnie_table));
		memcpy(mdnie->sequence_buffer, table->seq[trans_info->index].cmd, table->seq[trans_info->index].len);
		mdnie->table_buffer.seq[trans_info->index].cmd = mdnie->sequence_buffer;
		mdnie->table_buffer.seq[trans_info->index].cmd[trans_info->offset] = 0x0;
		mutex_unlock(&mdnie->lock);
		return &(mdnie->table_buffer);
	}

	mutex_unlock(&mdnie->lock);

	return table;
}

static void mdnie_update_sequence(struct mdnie_info *mdnie, struct mdnie_table *table)
{
	mdnie_write_table(mdnie, table);
}

static void mdnie_update(struct mdnie_info *mdnie)
{
	struct mdnie_table *table = NULL;
	struct mdnie_scr_info *scr_info = mdnie->tune->scr_info;

	if (!mdnie->enable) {
		dev_err(mdnie->dev, "mdnie state is off\n");
		return;
	}

	table = mdnie_find_table(mdnie);
	if (!IS_ERR_OR_NULL(table) && !IS_ERR_OR_NULL(table->name)) {
		mdnie_update_sequence(mdnie, table);
		dev_info(mdnie->dev, "%s\n", table->name);

		mdnie->white_r = table->seq[scr_info->index].cmd[scr_info->white_r];
		mdnie->white_g = table->seq[scr_info->index].cmd[scr_info->white_g];
		mdnie->white_b = table->seq[scr_info->index].cmd[scr_info->white_b];
	}
}

static void update_color_position(struct mdnie_info *mdnie, unsigned int idx)
{
	u8 mode, scenario;
	mdnie_t *wbuf;
	struct mdnie_scr_info *scr_info = mdnie->tune->scr_info;

	dev_info(mdnie->dev, "%s: idx=%d\n", __func__, idx);

	mutex_lock(&mdnie->lock);

	for (mode = 0; mode < MODE_MAX; mode++) {
		for (scenario = 0; scenario <= EMAIL_MODE; scenario++) {
			wbuf = mdnie->tune->main_table[scenario][mode].seq[scr_info->index].cmd;
			if (IS_ERR_OR_NULL(wbuf))
				continue;
			if ((scenario != EBOOK_MODE) && (mode != EBOOK)) {
				wbuf[scr_info->white_r] = mdnie->tune->coordinate_table[mode][idx * 3 + 0];
				wbuf[scr_info->white_g] = mdnie->tune->coordinate_table[mode][idx * 3 + 1];
				wbuf[scr_info->white_b] = mdnie->tune->coordinate_table[mode][idx * 3 + 2];
			}
			if (mode == AUTO && scenario == UI_MODE) {
				mdnie->white_default_r = mdnie->tune->coordinate_table[mode][idx * 3 + 0];
				mdnie->white_default_g = mdnie->tune->coordinate_table[mode][idx * 3 + 1];
				mdnie->white_default_b = mdnie->tune->coordinate_table[mode][idx * 3 + 2];
				dev_info(mdnie->dev, "%s, white_default_r %d, white_default_g %d, white_default_b %d\n",
				__func__, mdnie->white_default_r, mdnie->white_default_g, mdnie->white_default_b);
			}
		}
	}

	mutex_unlock(&mdnie->lock);
}

static int mdnie_calibration(int *r)
{
	int ret = 0;

	if (r[1] > 0) {
		if (r[3] > 0)
			ret = 3;
		else
			ret = (r[4] < 0) ? 1 : 2;
	} else {
		if (r[2] < 0) {
			if (r[3] > 0)
				ret = 9;
			else
				ret = (r[4] < 0) ? 7 : 8;
		} else {
			if (r[3] > 0)
				ret = 6;
			else
				ret = (r[4] < 0) ? 4 : 5;
		}
	}

	pr_info("%d, %d, %d, %d, tune%d\n", r[1], r[2], r[3], r[4], ret);

	return ret;
}

static int get_panel_coordinate(struct mdnie_info *mdnie, int *result)
{
	int ret = 0;

	unsigned short x, y;

	x = mdnie->coordinate[0];
	y = mdnie->coordinate[1];

	if (!(x || y)) {
		dev_info(mdnie->dev, "This panel do not need to adjust coordinate\n");
		ret = -EINVAL;
		goto skip_color_correction;
	}

	result[COLOR_OFFSET_FUNC_F1] = mdnie->tune->color_offset[COLOR_OFFSET_FUNC_F1](x, y);
	result[COLOR_OFFSET_FUNC_F2] = mdnie->tune->color_offset[COLOR_OFFSET_FUNC_F2](x, y);
	result[COLOR_OFFSET_FUNC_F3] = mdnie->tune->color_offset[COLOR_OFFSET_FUNC_F3](x, y);
	result[COLOR_OFFSET_FUNC_F4] = mdnie->tune->color_offset[COLOR_OFFSET_FUNC_F4](x, y);

	ret = mdnie_calibration(result);
	dev_info(mdnie->dev, "%s: %d, %d, idx=%d\n", __func__, x, y, ret);

skip_color_correction:
	mdnie->color_correction = 1;

	return ret;
}

static ssize_t mode_show(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	struct mdnie_info *mdnie = dev_get_drvdata(dev);

	return sprintf(buf, "%d\n", mdnie->mode);
}

static ssize_t mode_store(struct device *dev,
		struct device_attribute *attr, const char *buf, size_t count)
{
	struct mdnie_info *mdnie = dev_get_drvdata(dev);
	unsigned int value = 0;
	int ret;
	int result[COLOR_OFFSET_FUNC_MAX] = {0,};

	ret = kstrtouint(buf, 0, &value);
	if (ret < 0)
		return ret;

	dev_info(dev, "%s: value=%d\n", __func__, value);

	if (value >= MODE_MAX) {
		value = STANDARD;
		return -EINVAL;
	}

	mutex_lock(&mdnie->lock);
	mdnie->mode = value;
	mutex_unlock(&mdnie->lock);

	if (!mdnie->color_correction) {
		ret = get_panel_coordinate(mdnie, result);
		if (ret > 0)
			update_color_position(mdnie, ret);
	}

	mdnie_update(mdnie);

	return count;
}


static ssize_t scenario_show(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	struct mdnie_info *mdnie = dev_get_drvdata(dev);

	return sprintf(buf, "%d\n", mdnie->scenario);
}

static ssize_t scenario_store(struct device *dev,
		struct device_attribute *attr, const char *buf, size_t count)
{
	struct mdnie_info *mdnie = dev_get_drvdata(dev);
	unsigned int value;
	int ret;

	ret = kstrtouint(buf, 0, &value);
	if (ret < 0)
		return ret;

	dev_info(dev, "%s: value=%d\n", __func__, value);

	if (!SCENARIO_IS_VALID(value))
		value = UI_MODE;

	mutex_lock(&mdnie->lock);
	mdnie->scenario = value;
	mutex_unlock(&mdnie->lock);

	mdnie_update(mdnie);

	return count;
}

static ssize_t accessibility_show(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	struct mdnie_info *mdnie = dev_get_drvdata(dev);

	return sprintf(buf, "%d\n", mdnie->accessibility);
}

static ssize_t accessibility_store(struct device *dev,
		struct device_attribute *attr, const char *buf, size_t count)
{
	struct mdnie_info *mdnie = dev_get_drvdata(dev);
	int value;
	unsigned int s[12] = {0, }, i = 0;
	int ret;
	mdnie_t *wbuf;
	struct mdnie_scr_info *scr_info = mdnie->tune->scr_info;

	ret = sscanf(buf, "%8d %8x %8x %8x %8x %8x %8x %8x %8x %8x %8x %8x %8x",
		&value, &s[0], &s[1], &s[2], &s[3],
		&s[4], &s[5], &s[6], &s[7], &s[8], &s[9], &s[10], &s[11]);

	dev_info(dev, "%s: value: %d, cnt: %d\n", __func__, value, ret);

	if (ret < 0)
		return ret;
	else {
		if (value >= ACCESSIBILITY_MAX)
			value = ACCESSIBILITY_OFF;

		mutex_lock(&mdnie->lock);
		mdnie->accessibility = value;
		if (value == COLOR_BLIND || value == COLOR_BLIND_HBM) {
			if (ret > ARRAY_SIZE(s) + 1) {
				mutex_unlock(&mdnie->lock);
				return -EINVAL;
			}
			wbuf = &mdnie->tune->accessibility_table[value].seq[scr_info->index].cmd[scr_info->color_blind];
			while (i < ret - 1) {
				wbuf[i * 2 + 0] = GET_LSB_8BIT(s[i]);
				wbuf[i * 2 + 1] = GET_MSB_8BIT(s[i]);
				i++;
			}

			dev_info(dev, "%s: %s\n", __func__, buf);
		}
		mutex_unlock(&mdnie->lock);

		mdnie_update(mdnie);
	}

	return count;
}

static ssize_t color_correct_show(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	struct mdnie_info *mdnie = dev_get_drvdata(dev);
	char *pos = buf;
	int i, idx, result[COLOR_OFFSET_FUNC_MAX] = {0,};

	if (!mdnie->color_correction)
		return -EINVAL;

	idx = get_panel_coordinate(mdnie, result);

	for (i = COLOR_OFFSET_FUNC_F1; i < COLOR_OFFSET_FUNC_MAX; i++)
		pos += sprintf(pos, "f%d: %d, ", i, result[i]);
	pos += sprintf(pos, "tune%d\n", idx);

	return pos - buf;
}

static ssize_t bypass_show(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	struct mdnie_info *mdnie = dev_get_drvdata(dev);

	return sprintf(buf, "%d\n", mdnie->bypass);
}

static ssize_t bypass_store(struct device *dev,
		struct device_attribute *attr, const char *buf, size_t count)
{
	struct mdnie_info *mdnie = dev_get_drvdata(dev);
	struct mdnie_table *table = NULL;
	unsigned int value;
	int ret;

	ret = kstrtouint(buf, 0, &value);

	dev_info(dev, "%s: value=%d\n", __func__, value);

	if (ret < 0)
		return ret;
	else {
		if (value >= BYPASS_MAX)
			value = BYPASS_OFF;

		value = (value) ? BYPASS_ON : BYPASS_OFF;

		mutex_lock(&mdnie->lock);
		mdnie->bypass = value;
		mutex_unlock(&mdnie->lock);

		table = &mdnie->tune->bypass_table[value];
		if (!IS_ERR_OR_NULL(table)) {
			mdnie_write_table(mdnie, table);
			dev_info(mdnie->dev, "%s\n", table->name);
		}
	}

	return count;
}

static ssize_t lux_show(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	struct mdnie_info *mdnie = dev_get_drvdata(dev);

	return sprintf(buf, "%d\n", mdnie->hbm);
}

static ssize_t lux_store(struct device *dev,
		struct device_attribute *attr, const char *buf, size_t count)
{
	struct mdnie_info *mdnie = dev_get_drvdata(dev);
	unsigned int hbm = 0, update = 0;
	int ret, value;

	ret = kstrtoint(buf, 0, &value);
	if (ret < 0)
		return ret;

	if (!mdnie->tune->get_hbm_index)
		return ret;

	mutex_lock(&mdnie->lock);
	hbm = mdnie->tune->get_hbm_index(value);
	update = (mdnie->hbm != hbm) ? 1 : 0;
	mdnie->hbm = update ? hbm : mdnie->hbm;
	mutex_unlock(&mdnie->lock);

	if (update) {
		dev_info(dev, "%s: %d\n", __func__, value);
		mdnie_update(mdnie);
	}

	return count;
}

/* Temporary solution: Do not use this sysfs as official purpose */
static ssize_t mdnie_show(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	struct mdnie_info *mdnie = dev_get_drvdata(dev);
	char *pos = buf;
	struct mdnie_table *table = NULL;
	int i, j;
	u8 *buffer;

	if (!mdnie->enable) {
		dev_err(mdnie->dev, "mdnie state is off\n");
		goto exit;
	}

	table = mdnie_find_table(mdnie);

	for (i = 0; table->seq[i].len; i++) {
		if (IS_ERR_OR_NULL(table->seq[i].cmd)) {
			dev_err(mdnie->dev, "mdnie sequence %s %dth command is null,\n", table->name, i);
			goto exit;
		}
	}

	pos += sprintf(pos, "+ %s\n", table->name);

	for (j = 0; table->seq[j].len; j++) {
		if (!table->update_flag[j]) {
			mdnie->ops.write(mdnie->data, &table->seq[j], 1);
			continue;
		}

		buffer = kzalloc(table->seq[j].len, GFP_KERNEL);

		mdnie->ops.read(mdnie->data, table->seq[j].cmd[0], buffer, table->seq[j].len - 1);

		pos += sprintf(pos, "  0:\t0x%02x\t0x%02x\n", table->seq[j].cmd[0], table->seq[j].cmd[0]);
		for (i = 0; i < table->seq[j].len - 1; i++) {
			pos += sprintf(pos, "%3d:\t0x%02x\t0x%02x", i + 1, table->seq[j].cmd[i+1], buffer[i]);
			if (table->seq[j].cmd[i+1] != buffer[i])
				pos += sprintf(pos, "\t(X)");
			pos += sprintf(pos, "\n");
		}

		kfree(buffer);
	}

	pos += sprintf(pos, "- %s\n", table->name);

exit:
	return pos - buf;
}

static ssize_t sensorRGB_show(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	struct mdnie_info *mdnie = dev_get_drvdata(dev);

	return sprintf(buf, "%d %d %d\n", mdnie->white_r, mdnie->white_g, mdnie->white_b);
}

static ssize_t sensorRGB_store(struct device *dev,
		struct device_attribute *attr, const char *buf, size_t count)
{
	struct mdnie_info *mdnie = dev_get_drvdata(dev);
	struct mdnie_table *table = NULL;
	unsigned int white_red, white_green, white_blue;
	int ret;
	struct mdnie_scr_info *scr_info = mdnie->tune->scr_info;

	ret = sscanf(buf, "%8d %8d %8d",
		&white_red, &white_green, &white_blue);
	if (ret < 0)
		return ret;

	if (mdnie->enable) {
		dev_info(dev, "%s: %d, %d, %d\n", __func__, white_red, white_green, white_blue);

		table = mdnie_find_table(mdnie);

		memcpy(&(mdnie->table_buffer),
			table, sizeof(struct mdnie_table));
		memcpy(mdnie->sequence_buffer,
			table->seq[scr_info->index].cmd,
			table->seq[scr_info->index].len);
		mdnie->table_buffer.seq[scr_info->index].cmd
			= mdnie->sequence_buffer;

		mdnie->table_buffer.seq[scr_info->index].cmd
			[scr_info->white_r] = (unsigned char)white_red;
		mdnie->table_buffer.seq[scr_info->index].cmd
			[scr_info->white_g] = (unsigned char)white_green;
		mdnie->table_buffer.seq[scr_info->index].cmd
			[scr_info->white_b] = (unsigned char)white_blue;

		mdnie->white_r = white_red;
		mdnie->white_g = white_green;
		mdnie->white_b = white_blue;

		mdnie_update_sequence(mdnie, &(mdnie->table_buffer));

		table->seq[scr_info->index].cmd[scr_info->white_r] = (unsigned char)white_red;
		table->seq[scr_info->index].cmd[scr_info->white_g] = (unsigned char)white_green;
		table->seq[scr_info->index].cmd[scr_info->white_b] = (unsigned char)white_blue; 	
	
	}

	return count;
}

static ssize_t whiteRGB_show(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	struct mdnie_info *mdnie = dev_get_drvdata(dev);

	return sprintf(buf, "%d %d %d\n", mdnie->white_balance_r, mdnie->white_balance_g, mdnie->white_balance_b);
}

static ssize_t whiteRGB_store(struct device *dev,
		struct device_attribute *attr, const char *buf, size_t count)
{
	struct mdnie_info *mdnie = dev_get_drvdata(dev);
	mdnie_t *wbuf;
	u8 scenario;
	int white_red, white_green, white_blue;
	int ret;
	struct mdnie_scr_info *scr_info = mdnie->tune->scr_info;

	ret = sscanf(buf, "%8d %8d %8d",
		&white_red, &white_green, &white_blue);
	if (ret < 0)
		return ret;

	dev_info(dev, "%s, white_r %d, white_g %d, white_b %d\n",
		__func__, white_red, white_green, white_blue);

	if((white_red <= 0 && white_red >= -30) && (white_green <= 0 && white_green >= -30) && (white_blue <= 0 && white_blue >= -30)) {
		mutex_lock(&mdnie->lock);

		if(mdnie->mode == AUTO) {
			if(mdnie->ldu == 0) {
				mdnie->white_ldu_r = mdnie->white_default_r;
				mdnie->white_ldu_g = mdnie->white_default_g;
				mdnie->white_ldu_b = mdnie->white_default_b;
			}
			for (scenario = 0; scenario < SCENARIO_MAX; scenario++) {
				wbuf = mdnie->tune->main_table[scenario][AUTO].seq[scr_info->index].cmd;
				if (IS_ERR_OR_NULL(wbuf))
					continue;
				if (scenario != EBOOK_MODE) {
					wbuf[scr_info->white_r] = (unsigned char)(mdnie->white_ldu_r + white_red);
					wbuf[scr_info->white_g] = (unsigned char)(mdnie->white_ldu_g + white_green);
					wbuf[scr_info->white_b] = (unsigned char)(mdnie->white_ldu_b + white_blue);
					mdnie->white_balance_r = white_red;
					mdnie->white_balance_g = white_green;
					mdnie->white_balance_b = white_blue;
				}
			}
			wbuf = mdnie->tune->dmb_table[AUTO].seq[scr_info->index].cmd;
			if (!IS_ERR_OR_NULL(wbuf)) {
				wbuf[scr_info->white_r] = (unsigned char)(mdnie->white_ldu_r + white_red);
				wbuf[scr_info->white_g] = (unsigned char)(mdnie->white_ldu_g + white_green);
				wbuf[scr_info->white_b] = (unsigned char)(mdnie->white_ldu_b + white_blue);
				mdnie->white_balance_r = white_red;
				mdnie->white_balance_g = white_green;
				mdnie->white_balance_b = white_blue;
			}
		}
		mutex_unlock(&mdnie->lock);
		mdnie_update(mdnie);
	}

	return count;
}

static ssize_t night_mode_show(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	struct mdnie_info *mdnie = dev_get_drvdata(dev);

	return sprintf(buf, "%d %d\n", mdnie->night_mode, mdnie->night_mode_level);
}

static ssize_t night_mode_store(struct device *dev,
		struct device_attribute *attr, const char *buf, size_t count)
{
	struct mdnie_info *mdnie = dev_get_drvdata(dev);
	int enable, level, base_index;
	int i;
	int ret;
	mdnie_t *wbuf;
	struct mdnie_scr_info *scr_info = mdnie->tune->scr_info;

	ret = sscanf(buf, "%8d %8d", &enable, &level);

	if (ret < 0)
		return ret;
	else {
		if(mdnie->tune->night_table != NULL) {
			mutex_lock(&mdnie->lock);

			if ((enable == NIGHT_MODE_ON) && ((level >= 0) && (level < mdnie->tune->night_info->index_max_num)))
			{
				wbuf = &mdnie->tune->night_table[NIGHT_MODE_ON].seq[scr_info->index].cmd[scr_info->color_blind];
				base_index = mdnie->tune->night_info->index_size * level;
				for(i=0;i<mdnie->tune->night_info->index_size;i++) {
					wbuf[i] = mdnie->tune->night_mode_table[base_index + i];
				}
				mdnie->night_mode = NIGHT_MODE_ON;
				mdnie->night_mode_level = level;
			}
			else
			{
				mdnie->night_mode = NIGHT_MODE_OFF;
			}
			mutex_unlock(&mdnie->lock);

			mdnie_update(mdnie);
		}
	}

	return count;
}


static ssize_t mdnie_ldu_show(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	struct mdnie_info *mdnie = dev_get_drvdata(dev);

	return sprintf(buf, "%d %d %d\n", mdnie->white_r, mdnie->white_g, mdnie->white_b);
}

static ssize_t mdnie_ldu_store(struct device *dev,
		struct device_attribute *attr, const char *buf, size_t count)
{
	struct mdnie_info *mdnie = dev_get_drvdata(dev);
	mdnie_t *wbuf;
	u8 mode, scenario;
	int idx;
	int ret;
	struct mdnie_scr_info *scr_info = mdnie->tune->scr_info;

	ret = kstrtoint(buf, 10, &idx);
	if (ret < 0)
		return ret;

	if ((mdnie->tune->max_adjust_ldu != 0) && (mdnie->tune->adjust_ldu_table != NULL)) {
		if ((idx >= 0) && (idx < mdnie->tune->max_adjust_ldu)) {
			mutex_lock(&mdnie->lock);
			mdnie->ldu = idx;
			for (mode = 0; mode < MODE_MAX; mode++) {
				for (scenario = 0; scenario <= EMAIL_MODE; scenario++) {
					wbuf = mdnie->tune->main_table[scenario][mode].seq[scr_info->index].cmd;
					if (IS_ERR_OR_NULL(wbuf))
						continue;
					if (scenario != EBOOK_MODE) {
						if(mode == AUTO) {
							wbuf[scr_info->white_r] = mdnie->tune->adjust_ldu_table[mode][idx * 3 + 0] + mdnie->white_balance_r;
							wbuf[scr_info->white_g] = mdnie->tune->adjust_ldu_table[mode][idx * 3 + 1] + mdnie->white_balance_g;
							wbuf[scr_info->white_b] = mdnie->tune->adjust_ldu_table[mode][idx * 3 + 2] + mdnie->white_balance_b;
							mdnie->white_ldu_r = mdnie->tune->adjust_ldu_table[mode][idx * 3 + 0];
							mdnie->white_ldu_g = mdnie->tune->adjust_ldu_table[mode][idx * 3 + 1];
							mdnie->white_ldu_b = mdnie->tune->adjust_ldu_table[mode][idx * 3 + 2];
						}
						else{
							wbuf[scr_info->white_r] = mdnie->tune->adjust_ldu_table[mode][idx * 3 + 0];
							wbuf[scr_info->white_g] = mdnie->tune->adjust_ldu_table[mode][idx * 3 + 1];
							wbuf[scr_info->white_b] = mdnie->tune->adjust_ldu_table[mode][idx * 3 + 2];
						}
					}
				}
			}
			mutex_unlock(&mdnie->lock);
			mdnie_update(mdnie);
		}
	}

	return count;
}

static ssize_t light_notification_show(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	struct mdnie_info *mdnie = dev_get_drvdata(dev);

	return sprintf(buf, "%d\n", mdnie->light_notification);
}

static ssize_t light_notification_store(struct device *dev,
		struct device_attribute *attr, const char *buf, size_t count)
{
	struct mdnie_info *mdnie = dev_get_drvdata(dev);
	unsigned int value;
	int ret;

	ret = kstrtouint(buf, 0, &value);

	dev_info(dev, "%s: value=%d\n", __func__, value);

	if (ret < 0)
		return ret;
	else {
		if (value >= LIGHT_NOTIFICATION_MAX)
			value = LIGHT_NOTIFICATION_OFF;

		value = (value) ? LIGHT_NOTIFICATION_ON : LIGHT_NOTIFICATION_OFF;

		mutex_lock(&mdnie->lock);
		mdnie->light_notification = value;
		mutex_unlock(&mdnie->lock);

		mdnie_update(mdnie);
	}

	return count;
}

#ifdef CONFIG_LCD_HMT
static ssize_t hmtColorTemp_show(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	struct mdnie_info *mdnie = dev_get_drvdata(dev);

	return sprintf(buf, "hmt_mode: %d\n", mdnie->hmt_mode);
}

static ssize_t hmtColorTemp_store(struct device *dev,
		struct device_attribute *attr, const char *buf, size_t count)
{
	struct mdnie_info *mdnie = dev_get_drvdata(dev);
	unsigned int value;
	int ret;

	ret = kstrtouint(buf, 0, &value);
	if (ret < 0)
		return ret;


	if (value != mdnie->hmt_mode && value < HMT_MDNIE_MAX) {
		mutex_lock(&mdnie->lock);
		mdnie->hmt_mode = value;
		mutex_unlock(&mdnie->lock);
		mdnie_update(mdnie);
	}

	return count;
}
#endif

static DEVICE_ATTR(mode, 0664, mode_show, mode_store);
static DEVICE_ATTR(scenario, 0664, scenario_show, scenario_store);
static DEVICE_ATTR(accessibility, 0664, accessibility_show, accessibility_store);
static DEVICE_ATTR(color_correct, 0444, color_correct_show, NULL);
static DEVICE_ATTR(bypass, 0664, bypass_show, bypass_store);
static DEVICE_ATTR(lux, 0000, lux_show, lux_store);
static DEVICE_ATTR(mdnie, 0444, mdnie_show, NULL);
static DEVICE_ATTR(sensorRGB, 0664, sensorRGB_show, sensorRGB_store);
static DEVICE_ATTR(whiteRGB, 0664, whiteRGB_show, whiteRGB_store);
static DEVICE_ATTR(night_mode, 0664, night_mode_show, night_mode_store);
static DEVICE_ATTR(mdnie_ldu, 0664, mdnie_ldu_show, mdnie_ldu_store);
static DEVICE_ATTR(light_notification, 0664, light_notification_show, light_notification_store);
#ifdef CONFIG_LCD_HMT
static DEVICE_ATTR(hmt_color_temperature, 0664, hmtColorTemp_show, hmtColorTemp_store);
#endif


static struct attribute *mdnie_attrs[] = {
	&dev_attr_mode.attr,
	&dev_attr_scenario.attr,
	&dev_attr_accessibility.attr,
	&dev_attr_color_correct.attr,
	&dev_attr_bypass.attr,
	&dev_attr_lux.attr,
	&dev_attr_mdnie.attr,
	&dev_attr_sensorRGB.attr,
	&dev_attr_whiteRGB.attr,
	&dev_attr_night_mode.attr,
	&dev_attr_mdnie_ldu.attr,
	&dev_attr_light_notification.attr,
#ifdef CONFIG_LCD_HMT
	&dev_attr_hmt_color_temperature.attr,
#endif
	NULL,
};

ATTRIBUTE_GROUPS(mdnie);

static int fb_notifier_callback(struct notifier_block *self,
				 unsigned long event, void *data)
{
	struct mdnie_info *mdnie;
	struct fb_event *evdata = data;
	int fb_blank;

	switch (event) {
	case FB_EVENT_BLANK:
		break;
	default:
		return NOTIFY_DONE;
	}

	mdnie = container_of(self, struct mdnie_info, fb_notif);

	fb_blank = *(int *)evdata->data;

	dev_info(mdnie->dev, "%s: %d\n", __func__, fb_blank);

	if (evdata->info->node != 0)
		return 0;

	if (fb_blank == FB_BLANK_UNBLANK) {
		mutex_lock(&mdnie->lock);
		mdnie->light_notification = LIGHT_NOTIFICATION_OFF;
		mdnie->enable = 1;
		mutex_unlock(&mdnie->lock);

		mdnie_update(mdnie);
		if (mdnie->tune->trans_info->enable)
			mdnie->disable_trans_dimming = 0;
	} else if (fb_blank == FB_BLANK_POWERDOWN) {
		mutex_lock(&mdnie->lock);
		mdnie->enable = 0;
		if (mdnie->tune->trans_info->enable)
			mdnie->disable_trans_dimming = 1;
		mutex_unlock(&mdnie->lock);
	}

	return 0;
}

static int mdnie_register_fb(struct mdnie_info *mdnie)
{
	memset(&mdnie->fb_notif, 0, sizeof(mdnie->fb_notif));
	mdnie->fb_notif.notifier_call = fb_notifier_callback;
	return decon_register_notifier(&mdnie->fb_notif);
}

#ifdef CONFIG_DISPLAY_USE_INFO
static int dpui_notifier_callback(struct notifier_block *self,
				unsigned long event, void *data)
{
	struct mdnie_info *mdnie;
	char tbuf[MAX_DPUI_VAL_LEN];
	int size;

	mdnie = container_of(self, struct mdnie_info, dpui_notif);

	mutex_lock(&mdnie->lock);

	size = snprintf(tbuf, MAX_DPUI_VAL_LEN, "%d", mdnie->coordinate[0]);
	set_dpui_field(DPUI_KEY_WCRD_X, tbuf, size);
	size = snprintf(tbuf, MAX_DPUI_VAL_LEN, "%d", mdnie->coordinate[1]);
	set_dpui_field(DPUI_KEY_WCRD_Y, tbuf, size);

	size = snprintf(tbuf, MAX_DPUI_VAL_LEN, "%d", mdnie->white_balance_r);
	set_dpui_field(DPUI_KEY_WOFS_R, tbuf, size);
	size = snprintf(tbuf, MAX_DPUI_VAL_LEN, "%d", mdnie->white_balance_g);
	set_dpui_field(DPUI_KEY_WOFS_G, tbuf, size);
	size = snprintf(tbuf, MAX_DPUI_VAL_LEN, "%d", mdnie->white_balance_b);
	set_dpui_field(DPUI_KEY_WOFS_B, tbuf, size);

	mutex_unlock(&mdnie->lock);

	return 0;
}

static int mdnie_register_dpui(struct mdnie_info *mdnie)
{
	memset(&mdnie->dpui_notif, 0, sizeof(mdnie->dpui_notif));
	mdnie->dpui_notif.notifier_call = dpui_notifier_callback;
	return dpui_logging_register(&mdnie->dpui_notif, DPUI_TYPE_MDNIE);
}
#endif /* CONFIG_DISPLAY_USE_INFO */

int mdnie_register(struct device *p, void *data, mdnie_w w, mdnie_r r, unsigned int *coordinate, struct mdnie_tune *tune)
{
	int ret = 0;
	struct mdnie_info *mdnie;
	static unsigned int mdnie_no;

	if (IS_ERR_OR_NULL(mdnie_class)) {
		mdnie_class = class_create(THIS_MODULE, "mdnie");
		if (IS_ERR_OR_NULL(mdnie_class)) {
			pr_err("failed to create mdnie class\n");
			ret = -EINVAL;
			goto error0;
		}

		mdnie_class->dev_groups = mdnie_groups;
	}

	mdnie = kzalloc(sizeof(struct mdnie_info), GFP_KERNEL);
	if (!mdnie) {
		pr_err("failed to allocate mdnie\n");
		ret = -ENOMEM;
		goto error1;
	}

	mdnie->dev = device_create(mdnie_class, p, 0, &mdnie, !mdnie_no ? "mdnie" : "mdnie%d", mdnie_no);
	if (IS_ERR_OR_NULL(mdnie->dev)) {
		pr_err("failed to create mdnie device\n");
		ret = -EINVAL;
		goto error2;
	}

	mdnie_no++;
	mdnie->scenario = UI_MODE;
	mdnie->mode = STANDARD;
	mdnie->enable = 0;
	mdnie->tuning = 0;
	mdnie->accessibility = ACCESSIBILITY_OFF;
	mdnie->bypass = BYPASS_OFF;
	mdnie->bypass = BYPASS_OFF;
	mdnie->disable_trans_dimming = 0;
	mdnie->night_mode = NIGHT_MODE_OFF;
	mdnie->night_mode_level = 0;
	mdnie->ldu = 0;
	mdnie->light_notification = LIGHT_NOTIFICATION_OFF;

	mdnie->data = data;
	mdnie->ops.write = w;
	mdnie->ops.read = r;

	mdnie->coordinate[0] = coordinate ? coordinate[0] : 0;
	mdnie->coordinate[1] = coordinate ? coordinate[1] : 0;
	mdnie->tune = tune;

	mdnie->white_default_r = 255;
	mdnie->white_default_g = 255;
	mdnie->white_default_b = 255;
	mdnie->white_ldu_r = 255;
	mdnie->white_ldu_g = 255;
	mdnie->white_ldu_b = 255;
	mdnie->white_balance_r = 0;
	mdnie->white_balance_g = 0;
	mdnie->white_balance_b = 0;

	mutex_init(&mdnie->lock);
	mutex_init(&mdnie->dev_lock);

	dev_set_drvdata(mdnie->dev, mdnie);

	mdnie_register_fb(mdnie);
#ifdef CONFIG_DISPLAY_USE_INFO
	mdnie_register_dpui(mdnie);
#endif
	mdnie->enable = 1;
	mdnie_update(mdnie);

	dev_info(mdnie->dev, "registered successfully\n");

	return 0;

error2:
	kfree(mdnie);
error1:
	class_destroy(mdnie_class);
error0:
	return ret;
}


static int attr_find_and_store(struct device *dev,
	const char *name, const char *buf, size_t size)
{
	struct device_attribute *dev_attr;
	struct kernfs_node *kn;
	struct attribute *attr;

	kn = kernfs_find_and_get(dev->kobj.sd, name);
	if (!kn) {
		dev_info(dev, "%s: not found: %s\n", __func__, name);
		return 0;
	}

	attr = kn->priv;
	dev_attr = container_of(attr, struct device_attribute, attr);

	if (dev_attr && dev_attr->store)
		dev_attr->store(dev, dev_attr, buf, size);

	kernfs_put(kn);

	return 0;
}

ssize_t attr_store_for_each(struct class *cls,
	const char *name, const char *buf, size_t size)
{
	struct class_dev_iter iter;
	struct device *dev;
	int error = 0;
	struct class *class = cls;

	if (!class)
		return -EINVAL;
	if (!class->p) {
		WARN(1, "%s called for class '%s' before it was initialized",
		     __func__, class->name);
		return -EINVAL;
	}

	class_dev_iter_init(&iter, class, NULL, NULL);
	while ((dev = class_dev_iter_next(&iter))) {
		error = attr_find_and_store(dev, name, buf, size);
		if (error)
			break;
	}
	class_dev_iter_exit(&iter);

	return error;
}

struct class *get_mdnie_class(void)
{
	return mdnie_class;
}

