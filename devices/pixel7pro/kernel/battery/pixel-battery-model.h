/* SPDX-License-Identifier: GPL-2.0-only */
/* MAX77759 model restoration, following Google's max_m5.c ordering.
 * Profiles come from the bootloader's cheetah DT. Battery EEPROM is read-only;
 * no learned-state records, cycle counts or nonvolatile storage are written.
 * Included by pixel-battery.c after the supply and charger helpers.
 */

static int fg_write(struct pixel_battery *b, u8 reg, u16 value)
{
	u8 data[] = { reg, value & 0xff, value >> 8 };
	struct i2c_msg msg = { .addr = FG_ADDR, .len = sizeof(data), .buf = data };
	int ret = i2c_transfer(&b->adap, &msg, 1);

	return ret == 1 ? 0 : ret < 0 ? ret : -EIO;
}

static int fg_write_verify(struct pixel_battery *b, u8 reg, u16 value)
{
	u16 actual;
	int ret = fg_write(b, reg, value);

	if (!ret)
		ret = fg_read(b, reg, &actual);
	return ret ?: actual == value ? 0 : -EIO;
}

static int model_bus_find(struct device *dev, void *data)
{
	struct i2c_adapter *adap = i2c_verify_adapter(dev);

	/* i2c_for_each_dev holds the core lock: take the reference afterward. */
	if (adap && !strcmp(adap->name, "Pixel hsi2c_15"))
		*(int *)data = adap->nr;
	return 0;
}

static int model_battery_id(u8 *id)
{
	struct i2c_adapter *adap;
	u8 offset = 0x17; /* google_eeprom.c: BATT_EEPROM_TAG_BRID_OFFSET */
	struct i2c_msg msgs[] = {
		{ .addr = 0x50, .len = 1, .buf = &offset },
		{ .addr = 0x50, .flags = I2C_M_RD, .len = 1, .buf = id },
	};
	int nr = -1, ret;

	i2c_for_each_dev(&nr, model_bus_find);
	if (nr < 0)
		return -ENODEV;
	adap = i2c_get_adapter(nr);
	if (!adap)
		return -ENODEV;
	ret = i2c_transfer(adap, msgs, ARRAY_SIZE(msgs));
	i2c_put_adapter(adap);
	return ret == ARRAY_SIZE(msgs) ? 0 : ret < 0 ? ret : -EIO;
}

static int model_profile(u8 id, u16 model[48], u16 params[27], u32 *version)
{
	struct device_node *fg, *data, *config, *child;
	u32 rid, rsense;
	int ret = -ENODATA;

	if (id != 1 && id != 3)
		return -EOPNOTSUPP;
	fg = of_find_compatible_node(NULL, NULL, "maxim,max77759");
	if (!fg)
		return -ENODEV;
	if (of_property_read_u32(fg, "maxim,rsense-default", &rsense) || rsense != 500) {
		of_node_put(fg);
		return -EINVAL;
	}
	data = of_get_child_by_name(fg, "maxim,fg-data");
	config = of_get_child_by_name(data, "maxim,config");
	for_each_child_of_node(config, child) {
		if (of_property_read_u32(child, "maxim,batt-id-kohm", &rid) || rid != id)
			continue;
		if (of_property_count_u16_elems(child, "maxim,fg-model") == 48 &&
		    of_property_count_u16_elems(child, "maxim,fg-params") == 27 &&
		    !of_property_read_u16_array(child, "maxim,fg-model", model, 48) &&
		    !of_property_read_u16_array(child, "maxim,fg-params", params, 27) &&
		    !of_property_read_u32(child, "maxim,model-version", version))
			ret = 0;
		of_node_put(child);
		break;
	}
	of_node_put(config);
	of_node_put(data);
	of_node_put(fg);
	if (!ret && (*version != 1 || params[7] != 0x09c5 || params[26] != 0x2d00 ||
		     params[3] != 0x4217 || params[4] != 0x0090))
		ret = -EINVAL;
	return ret;
}

static int model_wait_clear(struct pixel_battery *b, u8 reg, u16 mask)
{
	u16 value;
	int i, ret;

	for (i = 0; i < 20; i++) {
		ret = fg_read(b, reg, &value);
		if (ret)
			return ret;
		if (!(value & mask))
			return 0;
		msleep(50);
	}
	return -ETIMEDOUT;
}

static int model_restore_locked(struct pixel_battery *b)
{
	/* max_m5_update_custom_parameters: exact order, including TaskPeriod. */
	static const u8 fields[][2] = {
		{0x28, 2}, {0x1d, 3}, {0xbb, 4}, {0x13, 5}, {0x35, 6},
		{0x18, 7}, {0x46, 8}, {0x45, 9}, {0x23, 10}, {0x3a, 11},
		{0x12, 12}, {0x22, 13}, {0x32, 14}, {0x42, 15},
		{0x38, 16}, {0x39, 17}, {0x3c, 26}, {0x1e, 18},
		{0x2c, 19}, {0x2d, 20}, {0x2b, 22},
	};
	u16 model[48], params[27], value, status, vcell, version_reg;
	u32 version;
	u8 id, cnfg, protection;
	int ret, i, temp, lock_ret;

	ret = model_battery_id(&id);
	if (!ret)
		ret = model_profile(id, model, params, &version);
	if (!ret)
		ret = fg_read(b, FG_STATUS, &status);
	if (ret)
		return ret;
	if (!(status & FG_STATUS_POR)) {
		/* Preserve an already initialized gauge, including learned state. */
		ret = fg_read(b, FG_DESIGNCAP, &value);
		if (!ret && value != params[7])
			ret = -EEXIST;
		return ret;
	}
	if (!usb_online(b) || battery_temp(b, &temp) || temp < 0 || temp >= TEMP_RESUME ||
	    fg_read(b, FG_VCELL, &vcell) || vcell * 625 / 8 < 3500000)
		return -EAGAIN;
	if (chg_read(b, CHG_CNFG_06, &protection) ||
	    FIELD_GET(CHGPROT, protection) != CHGPROT_UNLOCKED ||
	    chg_read(b, CHG_CNFG_00, &cnfg) ||
	    (FIELD_GET(CHG_MODE, cnfg) != CHG_MODE_BUCK &&
	     FIELD_GET(CHG_MODE, cnfg) != CHG_MODE_CHG_BUCK))
		return -EBUSY;
	ret = model_wait_clear(b, 0x3d, BIT(0)); /* FStat.DNR */
	if (!ret)
		ret = fg_read(b, 0xbb, &value);
	if (ret || value & BIT(5))
		return ret ?: -EBUSY;
	ret = chg_write(b, CHG_CNFG_00, (cnfg & ~CHG_MODE) | CHG_MODE_BUCK);
	if (ret)
		return ret;
	/* Keep charging paused on any partial load failure. */
	b->model_failed = true;

#define MODEL_WRITE(reg, val) do { \
	ret = fg_write_verify(b, (reg), (val)); \
	if (ret) goto relock; \
} while (0)
	/* Stock raw unlock is two LE16 words: 0x0059, 0x00c4. */
	ret = fg_write(b, 0x62, 0x0059);
	if (!ret)
		ret = fg_write(b, 0x63, 0x00c4);
	if (ret)
		goto relock;
	for (i = 0; i < ARRAY_SIZE(model); i++)
		MODEL_WRITE(0x80 + i, model[i]);
	ret = fg_write(b, 0x62, 0);
	if (!ret)
		ret = fg_write(b, 0x63, 0);
	if (ret)
		goto relock;
	for (i = 0; i < ARRAY_SIZE(model); i++) {
		ret = fg_read(b, 0x80 + i, &value);
		if (ret || value != 0xffff) {
			ret = ret ?: -EIO;
			goto relock;
		}
	}
	MODEL_WRITE(0x05, 0); /* RepCap */
	MODEL_WRITE(0x2a, params[1]);
	MODEL_WRITE(0x60, 0x80);
	ret = fg_read(b, 0xff, &value); /* VFSOC -> VFSOC0 */
	if (ret)
		goto relock;
	MODEL_WRITE(0x48, value);
	for (i = 0; i < ARRAY_SIZE(fields); i++)
		MODEL_WRITE(fields[i][0], params[fields[i][1]]);
	MODEL_WRITE(0x60, 0x80);
	MODEL_WRITE(0x04, params[23]);
	MODEL_WRITE(0xb6, params[10] * 75 / 100);
	MODEL_WRITE(0xb7, 0x600);
	MODEL_WRITE(0x49, params[24]);
	MODEL_WRITE(0x60, 0);
	MODEL_WRITE(0xb9, params[21]);
	MODEL_WRITE(0x29, params[25]);
	MODEL_WRITE(0x2e, 0x400); /* stock default CGain */
	ret = fg_read(b, 0x02, &version_reg);
	if (ret)
		goto relock;
	MODEL_WRITE(0x02, (version_reg & 0xff) | (version << 8));
	/* LdMdl clears itself, so do not demand an immediate exact readback. */
	ret = fg_write(b, 0xbb, params[4] | BIT(5));
	if (!ret)
		ret = model_wait_clear(b, 0xbb, BIT(5));
	if (ret)
		goto relock;
	ret = fg_read(b, 0x02, &value);
	if (ret || value >> 8 != version) {
		ret = ret ?: -EIO;
		goto relock;
	}
relock:
	/* All exit paths attempt both model and extra-config relocking. */
	lock_ret = fg_write_verify(b, 0x60, 0);
	if (!ret)
		ret = lock_ret;
	lock_ret = fg_write(b, 0x62, 0);
	if (!ret)
		ret = lock_ret;
	lock_ret = fg_write(b, 0x63, 0);
	if (!ret)
		ret = lock_ret;
#undef MODEL_WRITE
	/* Clear POR only after the model is loaded and both locks restored.
	 * Other status flags may change asynchronously; verify only POR.
	 */
	if (!ret)
		ret = fg_read(b, FG_STATUS, &status);
	if (!ret)
		ret = fg_write(b, FG_STATUS, status & ~FG_STATUS_POR);
	if (!ret)
		ret = fg_read(b, FG_STATUS, &status);
	if (!ret && status & FG_STATUS_POR)
		ret = -EIO;
	if (!ret) {
		b->model_failed = false;
		dev_info(b->dev, "restored battery profile %u version %u\n", id, version);
	} else {
		dev_err(b->dev, "battery model load failed (%d); charging remains paused\n", ret);
	}
	return ret;
}

static ssize_t restore_model_store(struct device *dev, struct device_attribute *attr,
				 const char *buf, size_t count)
{
	struct pixel_battery *b = dev_get_drvdata(dev);
	int ret;

	if (!sysfs_streq(buf, "1"))
		return -EINVAL;
	mutex_lock(&b->lock);
	ret = model_restore_locked(b);
	mutex_unlock(&b->lock);
	charger_update(b);
	power_supply_changed(b->battery);
	return ret ?: count;
}
static DEVICE_ATTR_WO(restore_model);

static struct attribute *pixel_model_attrs[] = {
	&dev_attr_restore_model.attr,
	NULL,
};
static const struct attribute_group pixel_model_group = { .attrs = pixel_model_attrs };
