/* Frozen nct6687.c anchors for inject_text. Not a live /usr/src tree. */

#define IOREGION_LENGTH 4

struct nct6687_data {
	const struct attribute_group *extra_groups[2];
	struct mutex update_lock;	/* used to protect sensor updates */
};

static struct nct6687_data *nct6687_update_device(struct device *dev)
{
	struct nct6687_data *data = dev_get_drvdata(dev);

	mutex_lock(&data->update_lock);

	if (time_after(jiffies, data->last_updated + HZ) || !data->valid)
	{
		nct6687_update_voltage(data);
		nct6687_update_temperatures(data);
		nct6687_update_fans(data);

		data->last_updated = jiffies;
		data->valid = true;
	}

	mutex_unlock(&data->update_lock);

	return data;
}

static int nct6687_probe(struct platform_device *pdev)
{
	nct6687_setup_voltages(data);
	if (nct6687_fan_config_type == FAN_CONFIG_MSI_ALT1 && msi_fan_brute_force)
		data->extra_groups[0] = &nct6687_fan_watchdog_group;
	return 0;
}
