/* For OEM project information
 *such as project name, hardware ID
 */

#include <linux/export.h>
#include <linux/module.h>
#include <linux/err.h>
#include <linux/of.h>
#include <linux/sys_soc.h>
#include <linux/slab.h>
#include <linux/stat.h>
#include <linux/string.h>
#include <linux/types.h>
#include <linux/project_info.h>
#include <linux/soc/qcom/smem.h>
#include <linux/gpio.h>
#include <soc/qcom/socinfo.h>
#include <linux/of_gpio.h>
#include <linux/platform_device.h>
#include <linux/pinctrl/consumer.h>
#include <linux/pinctrl/pinctrl.h>
#include <linux/pinctrl/machine.h>
#include <linux/soc/qcom/smem.h>
#include <linux/pstore.h>

static struct component_info component_info_desc[COMPONENT_MAX];
static struct kobject *project_info_kobj;
static struct project_info *project_info_desc;
extern void *panic_info;

static struct kobject *component_info;
static ssize_t project_info_get(struct kobject *kobj,
    struct kobj_attribute *attr, char *buf);
static ssize_t component_info_get(struct kobject *kobj,
    struct kobj_attribute *attr, char *buf);
static int op_aboard_read_gpio(void);

static struct kobj_attribute project_name_attribute =
	__ATTR(project_name, 0444, project_info_get, NULL);
static struct kobj_attribute hw_id_attribute =
	__ATTR(hw_id, 0444, project_info_get, NULL);
static struct kobj_attribute rf_id_v1_attribute =
	__ATTR(rf_id_v1, 0444, project_info_get, NULL);
static struct kobj_attribute rf_id_v2_attribute =
	__ATTR(rf_id_v2, 0444, project_info_get, NULL);
static struct kobj_attribute rf_id_v3_attribute =
	__ATTR(rf_id_v3, 0444, project_info_get, NULL);
static struct kobj_attribute ddr_manufacture_info_attribute =
	__ATTR(ddr_manufacture_info, 0444, project_info_get, NULL);
static struct kobj_attribute ddr_row_attribute =
	__ATTR(ddr_row, 0444, project_info_get, NULL);
static struct kobj_attribute ddr_column_attribute =
	__ATTR(ddr_column, 0444, project_info_get, NULL);
static struct kobj_attribute ddr_fw_version_attribute =
	__ATTR(ddr_fw_version, 0444, project_info_get, NULL);
static struct kobj_attribute ddr_reserve_info_attribute =
	__ATTR(ddr_reserve_info, 0444, project_info_get, NULL);
static struct kobj_attribute secboot_status_attribute =
	__ATTR(secboot_status, 0444, project_info_get, NULL);
static struct kobj_attribute platform_id_attribute =
	__ATTR(platform_id, 0444, project_info_get, NULL);
static struct kobj_attribute serialno_attribute =
	__ATTR(serialno, 0444, project_info_get, NULL);
static struct kobj_attribute aboard_id_attribute =
	__ATTR(aboard_id, 0444, project_info_get, NULL);
static struct kobj_attribute modem_attribute =
	__ATTR(modem, 0444, project_info_get, NULL);
static struct kobj_attribute operator_no_attribute =
	__ATTR(operator_no, 0444, project_info_get, NULL);
static struct kobj_attribute feature_id_attribute =
	__ATTR(feature_id, 0444, project_info_get, NULL);

#if 0
static DEVICE_ATTR(project_name, 0444, project_info_get, NULL);
static DEVICE_ATTR(hw_id, 0444, project_info_get, NULL);
static DEVICE_ATTR(rf_id_v1, 0444, project_info_get, NULL);
static DEVICE_ATTR(rf_id_v2, 0444, project_info_get, NULL);
static DEVICE_ATTR(rf_id_v3, 0444, project_info_get, NULL);
static DEVICE_ATTR(modem, 0444, project_info_get, NULL);
static DEVICE_ATTR(operator_no, 0444, project_info_get, NULL);
static DEVICE_ATTR(ddr_manufacture_info, 0444, project_info_get, NULL);
static DEVICE_ATTR(ddr_row, 0444, project_info_get, NULL);
static DEVICE_ATTR(ddr_column, 0444, project_info_get, NULL);
static DEVICE_ATTR(ddr_fw_version, 0444, project_info_get, NULL);
static DEVICE_ATTR(ddr_reserve_info, 0444, project_info_get, NULL);
static DEVICE_ATTR(secboot_status, 0444, project_info_get, NULL);
static DEVICE_ATTR(platform_id, 0444, project_info_get, NULL);
static DEVICE_ATTR(serialno, 0444, project_info_get, NULL);
static DEVICE_ATTR(feature_id, 0444, project_info_get, NULL);
static DEVICE_ATTR(aboard_id, 0444, project_info_get, NULL);
#endif 

uint8 get_secureboot_fuse_status(void)
{
    void __iomem *oem_config_base;
    uint8 secure_oem_config = 0;

    oem_config_base = ioremap(SECURE_BOOT1, 1);
    if (!oem_config_base)
        return -EINVAL;
    secure_oem_config = __raw_readb(oem_config_base);
    iounmap(oem_config_base);
    pr_debug("secure_oem_config 0x%x\n", secure_oem_config);

    return secure_oem_config;
}

static ssize_t project_info_get(struct kobject *kobj,
                struct kobj_attribute *attr,
                char *buf)
{
    if (project_info_desc) {
        if (attr == &project_name_attribute)
            return snprintf(buf, BUF_SIZE, "%s\n",
            project_info_desc->project_name);
        if (attr == &hw_id_attribute)
            return snprintf(buf, BUF_SIZE, "%d\n",
            project_info_desc->hw_version);
        if (attr == &rf_id_v1_attribute)
            return snprintf(buf, BUF_SIZE, "%d\n",
            project_info_desc->rf_v1);
        if (attr == &rf_id_v2_attribute)
            return snprintf(buf, BUF_SIZE, "%d\n",
            project_info_desc->rf_v2);
        if (attr == &rf_id_v3_attribute)
            return snprintf(buf, BUF_SIZE, "%d\n",
            project_info_desc->rf_v3);
        if (attr == &modem_attribute)
            return snprintf(buf, BUF_SIZE, "%d\n",
            project_info_desc->modem);
        if (attr == &operator_no_attribute)
            return snprintf(buf, BUF_SIZE, "%d\n",
            project_info_desc->operator);
        if (attr == &ddr_manufacture_info_attribute)
            return snprintf(buf, BUF_SIZE, "%d\n",
            project_info_desc->ddr_manufacture_info);
        if (attr == &ddr_row_attribute)
            return snprintf(buf, BUF_SIZE, "%d\n",
            project_info_desc->ddr_row);
        if (attr == &ddr_column_attribute)
            return snprintf(buf, BUF_SIZE, "%d\n",
            project_info_desc->ddr_column);
        if (attr == &ddr_fw_version_attribute)
            return snprintf(buf, BUF_SIZE, "%d\n",
            project_info_desc->ddr_fw_version);
        if (attr == &ddr_reserve_info_attribute)
            return snprintf(buf, BUF_SIZE, "%d\n",
            project_info_desc->ddr_reserve_info);
        if (attr == &secboot_status_attribute)
            return snprintf(buf, BUF_SIZE, "%d\n",
            get_secureboot_fuse_status());
        if (attr == &platform_id_attribute)
            return snprintf(buf, BUF_SIZE, "%d\n",
            project_info_desc->platform_id);
        if (attr == &serialno_attribute)
            return snprintf(buf, BUF_SIZE, "0x%x\n",
            socinfo_get_serial_number());
        if (attr == &feature_id_attribute)
            return snprintf(buf, BUF_SIZE, "%d\n",
            project_info_desc->feature_id);
        if (attr == &aboard_id_attribute)
            return snprintf(buf, BUF_SIZE, "%d\n",
            project_info_desc->a_board_version);
    }

    return -EINVAL;
}

#if 0
static struct attribute *project_info_sysfs_entries[] = {
    &dev_attr_project_name.attr,
    &dev_attr_hw_id.attr,
    &dev_attr_rf_id_v1.attr,
    &dev_attr_rf_id_v2.attr,
    &dev_attr_rf_id_v3.attr,
    &dev_attr_modem.attr,
    &dev_attr_operator_no.attr,
    &dev_attr_ddr_manufacture_info.attr,
    &dev_attr_ddr_row.attr,
    &dev_attr_ddr_column.attr,
    &dev_attr_ddr_fw_version.attr,
    &dev_attr_ddr_reserve_info.attr,
    &dev_attr_secboot_status.attr,
    &dev_attr_platform_id.attr,
    &dev_attr_serialno.attr,
    &dev_attr_feature_id.attr,
    &dev_attr_aboard_id.attr,
    NULL,
};

static struct attribute_group project_info_attr_group = {
    .attrs  = project_info_sysfs_entries,
};
#endif

static struct kobj_attribute ddr_attribute =
	__ATTR(ddr, 0444, component_info_get, NULL);
static struct kobj_attribute emmc_attribute =
	__ATTR(emmc, 0444, component_info_get, NULL);
static struct kobj_attribute f_camera_attribute =
	__ATTR(f_camera, 0444, component_info_get, NULL);
static struct kobj_attribute r_camera_attribute =
	__ATTR(r_camera, 0444, component_info_get, NULL);
static struct kobj_attribute second_r_camera_attribute =
	__ATTR(second_r_camera, 0444, component_info_get, NULL);
static struct kobj_attribute third_r_camera_attribute =
	__ATTR(third_r_camera, 0444, component_info_get, NULL);
static struct kobj_attribute r_ois_attribute =
	__ATTR(r_ois, 0444, component_info_get, NULL);
static struct kobj_attribute second_r_ois_attribute =
	__ATTR(second_r_ois, 0444, component_info_get, NULL);
static struct kobj_attribute tp_attribute =
	__ATTR(tp, 0444, component_info_get, NULL);
static struct kobj_attribute lcd_attribute =
	__ATTR(lcd, 0444, component_info_get, NULL);
static struct kobj_attribute wcn_attribute =
	__ATTR(wcn, 0444, component_info_get, NULL);
static struct kobj_attribute l_sensor_attribute =
	__ATTR(l_sensor, 0444, component_info_get, NULL);
static struct kobj_attribute g_sensor_attribute =
	__ATTR(g_sensor, 0444, component_info_get, NULL);
static struct kobj_attribute m_sensor_attribute =
	__ATTR(m_sensor, 0444, component_info_get, NULL);
static struct kobj_attribute gyro_attribute =
	__ATTR(gyro, 0444, component_info_get, NULL);
static struct kobj_attribute backlight_attribute =
	__ATTR(backlight, 0444, component_info_get, NULL);
static struct kobj_attribute mainboard_attribute =
	__ATTR(mainboard, 0444, component_info_get, NULL);
static struct kobj_attribute fingerprints_attribute =
	__ATTR(fingerprints, 0444, component_info_get, NULL);
static struct kobj_attribute touch_key_attribute =
	__ATTR(touch_key, 0444, component_info_get, NULL);
static struct kobj_attribute ufs_attribute =
	__ATTR(ufs, 0444, component_info_get, NULL);
static struct kobj_attribute Aboard_attribute =
	__ATTR(Aboard, 0444, component_info_get, NULL);
static struct kobj_attribute nfc_attribute =
	__ATTR(nfc, 0444, component_info_get, NULL);
static struct kobj_attribute fast_charge_attribute =
	__ATTR(fast_charge, 0444, component_info_get, NULL);
static struct kobj_attribute cpu_attribute =
	__ATTR(cpu, 0444, component_info_get, NULL);
static struct kobj_attribute rf_version_attribute =
	__ATTR(rf_version, 0444, component_info_get, NULL);
static struct kobj_attribute r_module_attribute =
	__ATTR(r_module, 0444, component_info_get, NULL);
static struct kobj_attribute f_module_attribute =
	__ATTR(f_module, 0444, component_info_get, NULL);

struct attribute *attrs[] = {
	&project_name_attribute.attr,
	&hw_id_attribute.attr,
	&rf_id_v1_attribute.attr,
	&rf_id_v2_attribute.attr,
	&rf_id_v3_attribute.attr,
	&ddr_manufacture_info_attribute.attr,
	&ddr_row_attribute.attr,
	&ddr_column_attribute.attr,
	&ddr_fw_version_attribute.attr,
	&ddr_reserve_info_attribute.attr,
	&secboot_status_attribute.attr,
	&platform_id_attribute.attr,
	&serialno_attribute.attr,
	&aboard_id_attribute.attr,
	&ddr_attribute.attr,
	&emmc_attribute.attr,
	&f_camera_attribute.attr,
	&r_camera_attribute.attr,
	&second_r_camera_attribute.attr,
	&third_r_camera_attribute.attr,
	&r_ois_attribute.attr,
	&second_r_ois_attribute.attr,
	&tp_attribute.attr,
	&lcd_attribute.attr,
	&wcn_attribute.attr,
	&l_sensor_attribute.attr,
	&g_sensor_attribute.attr,
	&m_sensor_attribute.attr,
	&gyro_attribute.attr,
	&backlight_attribute.attr,
	&mainboard_attribute.attr,
	&fingerprints_attribute.attr,
	&touch_key_attribute.attr,
	&ufs_attribute.attr,
	&Aboard_attribute.attr,
	&nfc_attribute.attr,
	&fast_charge_attribute.attr,
	&cpu_attribute.attr,
	&rf_version_attribute.attr,
	&r_module_attribute.attr,
	&f_module_attribute.attr,
	&modem_attribute.attr,
	&operator_no_attribute.attr,
	&feature_id_attribute.attr,
	NULL,
};

#if 0
static DEVICE_ATTR(ddr, 0444, component_info_get, NULL);
static DEVICE_ATTR(emmc, 0444, component_info_get, NULL);
static DEVICE_ATTR(f_camera, 0444, component_info_get, NULL);
static DEVICE_ATTR(r_camera, 0444, component_info_get, NULL);
static DEVICE_ATTR(second_r_camera, 0444, component_info_get, NULL);
static DEVICE_ATTR(third_r_camera, 0444, component_info_get, NULL);
static DEVICE_ATTR(r_ois, 0444, component_info_get, NULL);
static DEVICE_ATTR(second_r_ois, 0444, component_info_get, NULL);
static DEVICE_ATTR(r_module, 0444, component_info_get, NULL);
static DEVICE_ATTR(f_module, 0444, component_info_get, NULL);
static DEVICE_ATTR(tp, 0444, component_info_get, NULL);
static DEVICE_ATTR(lcd, 0444, component_info_get, NULL);
static DEVICE_ATTR(wcn, 0444, component_info_get, NULL);
static DEVICE_ATTR(l_sensor, 0444, component_info_get, NULL);
static DEVICE_ATTR(g_sensor, 0444, component_info_get, NULL);
static DEVICE_ATTR(m_sensor, 0444, component_info_get, NULL);
static DEVICE_ATTR(gyro, 0444, component_info_get, NULL);
static DEVICE_ATTR(backlight, 0444, component_info_get, NULL);
static DEVICE_ATTR(mainboard, 0444, component_info_get, NULL);
static DEVICE_ATTR(fingerprints, 0444, component_info_get, NULL);
static DEVICE_ATTR(touch_key, 0444, component_info_get, NULL);
static DEVICE_ATTR(ufs, 0444, component_info_get, NULL);
static DEVICE_ATTR(Aboard, 0444, component_info_get, NULL);
static DEVICE_ATTR(nfc, 0444, component_info_get, NULL);
static DEVICE_ATTR(fast_charge, 0444, component_info_get, NULL);
static DEVICE_ATTR(cpu, 0444, component_info_get, NULL);
static DEVICE_ATTR(rf_version, 0444, component_info_get, NULL);
#endif


char *get_component_version(enum COMPONENT_TYPE type)
{
    if (type >= COMPONENT_MAX) {
        pr_err("%s == type %d invalid\n", __func__, type);
        return "N/A";
    }
    return component_info_desc[type].version?:"N/A";
}

char *get_component_manufacture(enum COMPONENT_TYPE type)
{
    if (type >= COMPONENT_MAX) {
        pr_err("%s == type %d invalid\n", __func__, type);
        return "N/A";
    }
    return component_info_desc[type].manufacture?:"N/A";

}

int push_component_info(enum COMPONENT_TYPE type,
    char *version, char *manufacture)
{
    if (type >= COMPONENT_MAX)
        return -ENOMEM;
    component_info_desc[type].version = version;
    component_info_desc[type].manufacture = manufacture;

    return 0;
}
EXPORT_SYMBOL(push_component_info);

int reset_component_info(enum COMPONENT_TYPE type)
{
    if (type >= COMPONENT_MAX)
        return -ENOMEM;
    component_info_desc[type].version = NULL;
    component_info_desc[type].manufacture = NULL;

    return 0;
}
EXPORT_SYMBOL(reset_component_info);

#if 0
static struct attribute *component_info_sysfs_entries[] = {
    &dev_attr_ddr.attr,
    &dev_attr_emmc.attr,
    &dev_attr_f_camera.attr,
    &dev_attr_r_camera.attr,
    &dev_attr_second_r_camera.attr,
    &dev_attr_third_r_camera.attr,
    &dev_attr_r_ois.attr,
    &dev_attr_second_r_ois.attr,
    &dev_attr_r_module.attr,
    &dev_attr_f_module.attr,
    &dev_attr_tp.attr,
    &dev_attr_lcd.attr,
    &dev_attr_wcn.attr,
    &dev_attr_l_sensor.attr,
    &dev_attr_g_sensor.attr,
    &dev_attr_m_sensor.attr,
    &dev_attr_gyro.attr,
    &dev_attr_backlight.attr,
    &dev_attr_mainboard.attr,
    &dev_attr_fingerprints.attr,
    &dev_attr_touch_key.attr,
    &dev_attr_ufs.attr,
    &dev_attr_Aboard.attr,
    &dev_attr_nfc.attr,
    &dev_attr_fast_charge.attr,
    &dev_attr_cpu.attr,
    &dev_attr_rf_version.attr,
    NULL,
};

static struct attribute_group component_info_attr_group = {
    .attrs  = component_info_sysfs_entries,
};
#endif

static struct attribute_group attr_group = {
	.attrs = attrs,
};

static ssize_t component_info_get(struct kobject *kobj,
                struct kobj_attribute *attr,
                char *buf)
{
    if (attr == &ddr_attribute)
        return snprintf(buf, BUF_SIZE, "VER:\t%s\nMANU:\t%s\n",
        get_component_version(DDR),
        get_component_manufacture(DDR));
    if (attr == &emmc_attribute)
        return snprintf(buf, BUF_SIZE, "VER:\t%s\nMANU:\t%s\n",
        get_component_version(EMMC),
        get_component_manufacture(EMMC));
    if (attr == &f_camera_attribute)
        return snprintf(buf, BUF_SIZE, "VER:\t%s\nMANU:\t%s\n",
        get_component_version(F_CAMERA),
        get_component_manufacture(F_CAMERA));
    if (attr == &r_camera_attribute)
        return snprintf(buf, BUF_SIZE, "VER:\t%s\nMANU:\t%s\n",
        get_component_version(R_CAMERA),
        get_component_manufacture(R_CAMERA));
    if (attr == &second_r_camera_attribute)
        return snprintf(buf, BUF_SIZE, "VER:\t%s\nMANU:\t%s\n",
        get_component_version(SECOND_R_CAMERA),
        get_component_manufacture(SECOND_R_CAMERA));
    if (attr == &third_r_camera_attribute)
        return snprintf(buf, BUF_SIZE, "VER:\t%s\nMANU:\t%s\n",
        get_component_version(THIRD_R_CAMERA),
        get_component_manufacture(THIRD_R_CAMERA));
    if (attr == &r_ois_attribute)
        return snprintf(buf, BUF_SIZE, "VER:\t%s\nMANU:\t%s\n",
        get_component_version(R_OIS),
        get_component_manufacture(R_OIS));
    if (attr == &second_r_ois_attribute)
        return snprintf(buf, BUF_SIZE, "VER:\t%s\nMANU:\t%s\n",
        get_component_version(SECOND_R_OIS),
        get_component_manufacture(SECOND_R_OIS));
    if (attr == &r_module_attribute)
        return snprintf(buf, BUF_SIZE, "VER:\t%s\nMANU:\t%s\n",
        get_component_version(R_MODULE),
        get_component_manufacture(R_MODULE));
    if (attr == &f_module_attribute)
        return snprintf(buf, BUF_SIZE, "VER:\t%s\nMANU:\t%s\n",
        get_component_version(F_MODULE),
        get_component_manufacture(F_MODULE));
    if (attr == &tp_attribute)
        return snprintf(buf, BUF_SIZE, "VER:\t%s\nMANU:\t%s\n",
        get_component_version(TP),
        get_component_manufacture(TP));
    if (attr == &lcd_attribute)
        return snprintf(buf, BUF_SIZE, "VER:\t%s\nMANU:\t%s\n",
        get_component_version(LCD),
        get_component_manufacture(LCD));
    if (attr == &wcn_attribute)
        return snprintf(buf, BUF_SIZE, "VER:\t%s\nMANU:\t%s\n",
        get_component_version(WCN),
        get_component_manufacture(WCN));
    if (attr == &l_sensor_attribute)
        return snprintf(buf, BUF_SIZE, "VER:\t%s\nMANU:\t%s\n",
        get_component_version(I_SENSOR),
        get_component_manufacture(I_SENSOR));
    if (attr == &g_sensor_attribute)
        return snprintf(buf, BUF_SIZE, "VER:\t%s\nMANU:\t%s\n",
        get_component_version(G_SENSOR),
        get_component_manufacture(G_SENSOR));
    if (attr == &m_sensor_attribute)
        return snprintf(buf, BUF_SIZE, "VER:\t%s\nMANU:\t%s\n",
        get_component_version(M_SENSOR),
        get_component_manufacture(M_SENSOR));
    if (attr == &gyro_attribute)
        return snprintf(buf, BUF_SIZE, "VER:\t%s\nMANU:\t%s\n",
        get_component_version(GYRO),
        get_component_manufacture(GYRO));
    if (attr == &backlight_attribute)
        return snprintf(buf, BUF_SIZE, "VER:\t%s\nMANU:\t%s\n",
        get_component_version(BACKLIGHT),
        get_component_manufacture(BACKLIGHT));
    if (attr == &mainboard_attribute)
        return snprintf(buf, BUF_SIZE, "VER:\t%s\nMANU:\t%s\n",
        get_component_version(MAINBOARD),
        get_component_manufacture(MAINBOARD));
    if (attr == &fingerprints_attribute)
        return snprintf(buf, BUF_SIZE, "VER:\t%s\nMANU:\t%s\n",
        get_component_version(FINGERPRINTS),
        get_component_manufacture(FINGERPRINTS));
    if (attr == &touch_key_attribute)
        return snprintf(buf, BUF_SIZE, "VER:\t%s\nMANU:\t%s\n",
        get_component_version(TOUCH_KEY),
        get_component_manufacture(TOUCH_KEY));
    if (attr == &ufs_attribute)
        return snprintf(buf, BUF_SIZE, "VER:\t%s\nMANU:\t%s\n",
        get_component_version(UFS),
        get_component_manufacture(UFS));
    if (attr == &Aboard_attribute) {
        return snprintf(buf, BUF_SIZE, "VER:\t%s\nMANU:\t%s\n",
        get_component_version(ABOARD),
        get_component_manufacture(ABOARD));
    }
    if (attr == &nfc_attribute)
        return snprintf(buf, BUF_SIZE, "VER:\t%s\nMANU:\t%s\n",
        get_component_version(NFC),
        get_component_manufacture(NFC));
    if (attr == &fast_charge_attribute)
        return snprintf(buf, BUF_SIZE, "VER:\t%s\nMANU:\t%s\n",
        get_component_version(FAST_CHARGE),
        get_component_manufacture(FAST_CHARGE));
    if (attr == &cpu_attribute)
        return snprintf(buf, BUF_SIZE, "VER:\t%s\nMANU:\t%s\n",
        get_component_version(CPU),
        get_component_manufacture(CPU));
    if (attr == &rf_version_attribute)
        return snprintf(buf, BUF_SIZE, "VER:\t%s\nMANU:\t%s\n",
        get_component_version(RF_VERSION),
        get_component_manufacture(RF_VERSION));
    return -EINVAL;
}

static int __init project_info_init_sysfs(void)
{
    int error = 0;

    project_info_kobj = kobject_create_and_add("project_info", NULL);
    if (!project_info_kobj)
        return -ENOMEM;
    error = sysfs_create_group(project_info_kobj, &attr_group);
    if (error) {
        pr_err("project_info_init_sysfs project_info_attr_group failure\n");
        return error;
    }

    component_info = kobject_create_and_add("component_info",
        project_info_kobj);
    pr_info("project_info_init_sysfs success\n");
    if (!component_info)
        return -ENOMEM;

    error = sysfs_create_group(component_info, &attr_group);
    if (error) {
        pr_err("project_info_init_sysfs project_info_attr_group failure\n");
        return error;
    }
    return 0;
}

late_initcall(project_info_init_sysfs);

struct ddr_manufacture {
    int id;
    char name[20];
};
//ddr id and ddr name
static char ddr_version[32] = {0};
static char ddr_manufacture[20] = {0};
char ddr_manufacture_and_fw_verion[40] = {0};
static char cpu_type[20] = {0};

struct ddr_manufacture ddr_manufacture_list[] = {
    {1, "Samsung "},
    {2, "Qimonda "},
    {3, "Elpida "},
    {4, "Etpon "},
    {5, "Nanya "},
    {6, "Hynix "},
    {7, "Mosel "},
    {8, "Winbond "},
    {9, "Esmt "},
    {255, "Micron"},
    {0, "Unknown"},
};

struct cpu_list {
    int id;
    char name[20];
};

struct cpu_list cpu_list_msm[] = {
	{194, "MSM8974AC "},
	{217, "MSM8974AA "},
	{218, "MSM8974AB "},
	{207, "MSM8994 "},
	{246, "MSM8996 "},
	{292, "MSM8998 "},
	{302, "MSM8996L "},
	{305, "MSM8996SG "},
	{310, "MSM8996AU "},
	{321, "SDM845 "},
	{339, "SM8150 "},
	{361, "SM8150P_SDX55M "},
	{0, "Unknown"},
};

void get_ddr_manufacture_name(void)
{
    uint32 i, length;

    length = ARRAY_SIZE(ddr_manufacture_list);
    if (project_info_desc) {
        for (i = 0; i < length; i++) {
            if (ddr_manufacture_list[i].id ==
                project_info_desc->ddr_manufacture_info) {
                snprintf(ddr_manufacture, sizeof(ddr_manufacture), "%s",
                    ddr_manufacture_list[i].name);
                break;
            }
        }
    }
}

void get_cpu_type(void)
{
    uint32 i, length;

    length = ARRAY_SIZE(cpu_list_msm);
    if (project_info_desc) {
        for (i = 0; i < length; i++) {
            if (cpu_list_msm[i].id ==
                project_info_desc->platform_id) {
                snprintf(cpu_type, sizeof(cpu_type),
                    "%s", cpu_list_msm[i].name);
                break;
            }
        }
    }
}

static char mainboard_version[64] = {0};
static char mainboard_manufacture[8] = {'O',
    'N', 'E', 'P', 'L', 'U', 'S', '\0'};
static char Aboard_version[16] = {0};
static char rf_version[16] = {0};

struct a_board_version {
	int  version;
	char name[16];
};

struct main_board_info {
	int  prj_version;
	int  hw_version;
	char version_name[32];
};

struct a_board_version a_board_version_string_arry_gpio[] = {
	{0, "SAR"},
	{1, "Unknown"},
	{2, "Unknown"},
	{3, "NOSAR"},
};

struct main_board_info main_board_info_check[] = {
	{1, 11, "T0"},
	{1, 12, "EVT1"},
	{1, 13, "EVT2"},
	{1, 55, "AM EVT2"},
	{1, 54, "EU EVT2"},
	{1, 14, "EVT3"},
	{1, 53, "AM EVT3"},
	{1, 52, "EU EVT3"},
	{1, 15, "DVT"},
	{1, 21, "PVT"},
	{1, 22, "MP"},

	{2, 11, "T0"},
	{2, 11, "EVT1"},
	{2, 13, "CS EVT1"},
	{2, 14, "EVT2"},
	{2, 15, "DVT"},
	{2, 21, "PVT"},
	{2, 22, "MP"},

	{2, 42, "T0"},
	{2, 43, "EVT1"},
	{2, 44, "EVT2"},
	{2, 45, "DVT"},
	{2, 51, "PVT"},
	{2, 52, "MP"},

	{2, 24, "Sprint EVT"},

	{3, 11, "T0"},
	{3, 12, "EVT1"},
	{3, 13, "DVT"},
	{3, 14, "PVT"},
	{3, 15, "MP"},

	{4, 11, "T0"},
	{4, 12, "EVT1"},
	{4, 52, "EVT SEC"},
	{4, 13, "DVT"},
	{4, 14, "PVT"},
	{4, 15, "MP"},

	{5, 11, "T0"},
	{5, 12, "EVT1"},
	{5, 13, "DVT"},
	{5, 14, "PVT"},
	{5, 15, "MP"},

	{-1, -1, "Unknown"}
};

uint32 get_hw_version(void)
{
    size_t size;

    project_info_desc = qcom_smem_get(QCOM_SMEM_HOST_ANY,SMEM_PROJECT_INFO, &size);

    if (IS_ERR_OR_NULL(project_info_desc))
        pr_err("%s: get project_info failure\n", __func__);
    else {
        pr_err("%s: hw version: %d\n", __func__,
            project_info_desc->hw_version);
        return project_info_desc->hw_version;
    }
    return 0;
}

void dump_reason_init_smem(void)
{
    int ret;

    ret = qcom_smem_alloc(QCOM_SMEM_HOST_ANY,SMEM_DUMP_INFO,
                                  sizeof(struct dump_info));

    if (ret < 0 && ret != -EEXIST) {
          pr_err("%s:unable to allocate dp_info \n", __func__);
          return;
    }
}

int __init init_project_info(void)
{
    static bool project_info_init_done;
    int ddr_size = 0;
    size_t size;
    int i = 0;
    char *p = NULL;

    if (project_info_init_done)
        return 0;

    project_info_desc = qcom_smem_get(QCOM_SMEM_HOST_ANY,SMEM_PROJECT_INFO, &size);

    if (IS_ERR_OR_NULL(project_info_desc)) {
        pr_err("%s: get project_info failure\n", __func__);
        return -1;
    }
    pr_err("%s: project_name: %s hw_version: %d prj=%d rf_v1: %d rf_v2: %d: rf_v3: %d  paltform_id:%d\n",
        __func__, project_info_desc->project_name,
        project_info_desc->hw_version,
        project_info_desc->prj_version,
        project_info_desc->rf_v1,
        project_info_desc->rf_v2,
        project_info_desc->rf_v3,
        project_info_desc->platform_id);


    p = &main_board_info_check[ARRAY_SIZE(main_board_info_check)-1].version_name[0];

    for( i = 0 ; i < ARRAY_SIZE(main_board_info_check) ; i++ )
    {
        if(project_info_desc->prj_version == main_board_info_check[i].prj_version &&
           project_info_desc->hw_version  == main_board_info_check[i].hw_version )
        {
           p = &main_board_info_check[i].version_name[0];
           break;
        }
    }

    snprintf(mainboard_version, sizeof(mainboard_version), "%d %d %s %s ",
        project_info_desc->prj_version,project_info_desc->hw_version,
        project_info_desc->project_name, p);

    pr_err("board info: %s\n", mainboard_version);
    push_component_info(MAINBOARD,
        mainboard_version,
        mainboard_manufacture);

    snprintf(rf_version, sizeof(rf_version),  " %d",project_info_desc->rf_v1);
    push_component_info(RF_VERSION, rf_version, mainboard_manufacture);

    get_ddr_manufacture_name();

	/* approximate as ceiling of total pages */
	ddr_size = (totalram_pages() + (1 << 18) - 1) >> 18;

	snprintf(ddr_version, sizeof(ddr_version), "size_%dG_r_%d_c_%d",
		ddr_size, project_info_desc->ddr_row,
		project_info_desc->ddr_column);
	snprintf(ddr_manufacture_and_fw_verion,
		sizeof(ddr_manufacture_and_fw_verion),
		"%s%s %u.%u", ddr_manufacture,
		project_info_desc->ddr_reserve_info == 0x05 ? "20nm" :
		(project_info_desc->ddr_reserve_info == 0x06 ? "18nm" : " "),
		project_info_desc->ddr_fw_version >> 16,
		project_info_desc->ddr_fw_version & 0x0000FFFF);
	push_component_info(DDR, ddr_version, ddr_manufacture_and_fw_verion);

	get_cpu_type();
	push_component_info(CPU, cpu_type, "Qualcomm");
	project_info_init_done = true;
	dump_reason_init_smem();

	return 0;
}

struct aboard_data {
	int aboard_gpio_0;
	int aboard_gpio_1;
	int support_aboard_gpio_0;
	int support_aboard_gpio_1;
	struct pinctrl                      *pinctrl;
	struct pinctrl_state                *pinctrl_state_active;
	struct pinctrl_state                *pinctrl_state_sleep;
	struct device *dev;
};
static struct aboard_data *data;

static int op_aboard_request_named_gpio(const char *label, int *gpio)
{
    struct device *dev = data->dev;
    struct device_node *np = dev->of_node;
    int rc = of_get_named_gpio(np, label, 0);

    if (rc < 0) {
        dev_err(dev, "failed to get '%s'\n", label);
        *gpio = rc;
        return rc;
    }
    *gpio = rc;

    rc = devm_gpio_request(dev, *gpio, label);
    if (rc) {
        dev_err(dev, "failed to request gpio %d\n", *gpio);
        return rc;
    }

    dev_info(dev, "%s gpio: %d\n", label, *gpio);
    return 0;
}

static int op_aboard_read_gpio(void)
{
	int gpio0 = 0;
	int gpio1 = 0;

	if (data == NULL || IS_ERR_OR_NULL(project_info_desc))
		return 0;
	if (data->support_aboard_gpio_0 == 1)
		gpio0 = gpio_get_value(data->aboard_gpio_0);
	if (data->support_aboard_gpio_1 == 1)
		gpio1 = gpio_get_value(data->aboard_gpio_1);

	if (data->support_aboard_gpio_0 == 1 && data->support_aboard_gpio_1 == 1) {
		pr_err("%s: gpio0=%d gpio1=%d\n", __func__, gpio0, gpio1);

		if (gpio0 == 0 && gpio1 == 0)
			project_info_desc->a_board_version = 0;
		else if (gpio0 == 0 && gpio1 == 1)
			project_info_desc->a_board_version = 1;
		else if (gpio0 == 1 && gpio1 == 0)
			project_info_desc->a_board_version = 2;
		else
			project_info_desc->a_board_version = 3;
	} else if (data->support_aboard_gpio_0 == 1) {
		pr_err("%s: gpio0=%d\n", __func__, gpio0);
		project_info_desc->a_board_version = (gpio0 == 1 ? 0:3);
	} else if (data->support_aboard_gpio_1 == 1) {
		pr_err("%s: gpio1=%d\n", __func__, gpio1);
		project_info_desc->a_board_version = (gpio1 == 1 ? 0:3);
	}

	snprintf(Aboard_version, sizeof(Aboard_version), "%d", project_info_desc->a_board_version);

	push_component_info(ABOARD, Aboard_version, mainboard_manufacture);
	pr_err("%s: Aboard_gpio(%s)\n", __func__, Aboard_version);
	return 0;

}

static int oem_aboard_probe(struct platform_device *pdev)
{
	int rc = 0;
	struct device *dev = &pdev->dev;

	data = kzalloc(sizeof(struct aboard_data), GFP_KERNEL);
	if (!data) {
		rc = -ENOMEM;
		goto exit;
	}

	data->dev = dev;
	rc = op_aboard_request_named_gpio("oem,aboard-gpio-0", &data->aboard_gpio_0);
	if (rc)
		pr_err("%s: oem,aboard-gpio-0 fail\n", __func__);
	else
		data->support_aboard_gpio_0 = 1;


	rc = op_aboard_request_named_gpio("oem,aboard-gpio-1", &data->aboard_gpio_1);
	if (rc)
		pr_err("%s: oem,aboard-gpio-1 fail\n", __func__);
	else
		data->support_aboard_gpio_1 = 1;

	data->pinctrl = devm_pinctrl_get((data->dev));
	if (IS_ERR_OR_NULL(data->pinctrl)) {
		rc = PTR_ERR(data->pinctrl);
		pr_err("%s pinctrl error!\n", __func__);
		goto err_pinctrl_get;
	}

    data->pinctrl_state_active = pinctrl_lookup_state(data->pinctrl, "oem_aboard_active");

    if (IS_ERR_OR_NULL(data->pinctrl_state_active)) {
        rc = PTR_ERR(data->pinctrl_state_active);
        pr_err("%s pinctrl state active error!\n",__func__);
        goto err_pinctrl_lookup;
    }

    if (data->pinctrl) {
        rc = pinctrl_select_state(data->pinctrl,data->pinctrl_state_active);
    }
    if(data->support_aboard_gpio_0 == 1)
        gpio_direction_input(data->aboard_gpio_0);
    if(data->support_aboard_gpio_1 == 1)
        gpio_direction_input(data->aboard_gpio_1);
    op_aboard_read_gpio();

    data->pinctrl_state_sleep = pinctrl_lookup_state(data->pinctrl, "oem_aboard_sleep");

    if (data->pinctrl && !IS_ERR_OR_NULL(data->pinctrl_state_sleep)) {
        rc = pinctrl_select_state(data->pinctrl,data->pinctrl_state_sleep);
    }

    pr_err("%s: probe ok!\n", __func__);
    return 0;
    
err_pinctrl_lookup:
    devm_pinctrl_put(data->pinctrl);
err_pinctrl_get:
    data->pinctrl = NULL;
    kfree(data);
exit:
    pr_err("%s: probe Fail!\n", __func__);

    return rc;
}

static const struct of_device_id aboard_of_match[] = {
    { .compatible = "oem,aboard", },
    {}
};
MODULE_DEVICE_TABLE(of, aboard_of_match);

static struct platform_driver aboard_driver = {
    .driver = {
        .name       = "op_aboard",
        .owner      = THIS_MODULE,
        .of_match_table = aboard_of_match,
    },
    .probe = oem_aboard_probe,
};

static int __init init_project(void)
{
    int ret;

    ret = init_project_info();

    return ret;
}
static int __init init_aboard(void)
{
    int ret;

    ret = platform_driver_register(&aboard_driver);
    if (ret)
        pr_err("aboard_driver register failed: %d\n", ret);

    return ret;
}


subsys_initcall(init_project);
late_initcall(init_aboard);

