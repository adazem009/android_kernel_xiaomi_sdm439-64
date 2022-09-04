#include <linux/module.h>
#include <linux/init.h>
#include <linux/gpio.h>
#include <linux/slab.h>
#include <linux/kobject.h>
#include <linux/sysfs.h>
#include <linux/kernel.h>
#include <linux/interrupt.h>
#include <linux/platform_device.h>
#include <linux/of_address.h>
#include <linux/of_irq.h>
#include <linux/of_gpio.h>
#include <linux/of_pci.h>
#include <linux/of_platform.h>
#include <asm/irq.h>

enum DOCKSTATE {
   STATE_UNPLUG = 0,
   STATE_PLUGED,
   STATE_UNKNOW
};

struct dock_context {
    u32 irq_gpio;
    u32 gpio_active_flag;
    int irq_num;
    int plug_state;
    int force_update;
    struct delayed_work work;
    struct workqueue_struct *event_wq;
    struct device *dev;
    struct class *dock_class;
};

static struct dock_context *dock_context_info;

static void dock_report_work(struct work_struct *work)
{
    char *pluged[2] = { "DOCK_STATE=DOCKED", NULL };
    char *unpluged[2]    = { "DOCK_STATE=UNDOCKED", NULL };
    bool uevent_sent = false;
    int state = gpio_get_value_cansleep(dock_context_info->irq_gpio);
    state ^= 1; //0->1;1->0

    if (dock_context_info->plug_state != state || 1 == dock_context_info->force_update) {
        dock_context_info->plug_state = state;
        if (state & dock_context_info->gpio_active_flag) {
            pr_info("%s: sent uevent %s \n", __func__, pluged[0]);
            kobject_uevent_env(&dock_context_info->dev->kobj, KOBJ_CHANGE, pluged);
        } else {
            pr_info("%s: sent uevent %s \n", __func__, unpluged[0]);
            kobject_uevent_env(&dock_context_info->dev->kobj, KOBJ_CHANGE, unpluged);
        }
        if (1 == dock_context_info->force_update) {
            pr_info("Force sent uevent this time \n");
            dock_context_info->force_update = 0;
        }
        uevent_sent = true;
    }
    if (!uevent_sent) {
        pr_info("%s: did not send uevent (%d %d)\n", __func__,
            dock_context_info->plug_state, state);
    }
    enable_irq(dock_context_info->irq_num);
}

static irqreturn_t dock_interrupt(int irq, void *data)
{
    struct dock_context *di = data;
    disable_irq_nosync(irq);
    queue_delayed_work(di->event_wq, &di->work, msecs_to_jiffies(150));
    return IRQ_HANDLED;
}

static int dock_parse_dt(struct device *dev, struct dock_context *pdata)
{
    struct device_node *np = dev->of_node;

    pdata->irq_gpio = of_get_named_gpio_flags(np, "pogo,irq-gpio",
	0, &pdata->gpio_active_flag);
    if (pdata->irq_gpio < 0)
        return pdata->irq_gpio;

    pr_info("Dock driver irq_gpio=%d,gpio_active_flag=%u\n", pdata->irq_gpio, pdata->gpio_active_flag);
    return 0;
}

static ssize_t trigger_store(struct device *d,
				  struct device_attribute *attr,
				  const char *buf,
				  size_t n)
{
    long tmp;
    if (!kstrtol(buf, 0, &tmp) && 1 == tmp && NULL != dock_context_info) {
        //trigger to send a uevent
        dock_context_info->force_update = 1;
        queue_delayed_work(dock_context_info->event_wq, &dock_context_info->work, msecs_to_jiffies(50));
        return n;
    }
    pr_err("Dock driver trigger store failed\n");
    return -EINVAL;
}

static ssize_t trigger_show(struct device *d,
				 struct device_attribute *attr,
				 char *buf)
{
    if (NULL != dock_context_info) {
        return snprintf(buf, PAGE_SIZE, "%d\n",dock_context_info->plug_state);
    }
    pr_err("Dock driver trigger show failed\n");
    return -EINVAL;
}

static DEVICE_ATTR(trigger, 0664, trigger_show, trigger_store);

static int dock_detect_probe( struct platform_device *pdev)
{
    int ret = 0;
    int err;
    struct dock_context *context = NULL;

    pr_err("Dock driver probe start \n");

    if (pdev->dev.of_node) {
        context = kzalloc(sizeof(struct dock_context), GFP_KERNEL);		  
        if (!context) {
            pr_err("Dock driver failed to alloc memory for module data \n");
            return -ENOMEM;
        }
        err = dock_parse_dt(&pdev->dev, context);
        if (err) {
            dev_err(&pdev->dev, "Dock driver failed to parse dt data \n");
            goto free_context;
        }
    } else{
        pr_err("Dock driver failed to find dt data \n");
        return -ENOMEM;
    }

    if (gpio_is_valid(context->irq_gpio)) {
        err = gpio_request(context->irq_gpio, "dock-detect-gpio");
        if (err < 0) {
            pr_err("Dock driver gpio_request fail err = %d \n", err);
            goto free_context;
        }
        err = gpio_direction_input(context->irq_gpio);
        if (err < 0) {
            pr_err("Dock driver gpio_direction_input fail err = %d\n", err);
            goto free_gpio;
        }
        context->plug_state = gpio_get_value(context->irq_gpio);
        context->irq_num = gpio_to_irq(context->irq_gpio);
        err = devm_request_threaded_irq(&pdev->dev, context->irq_num, NULL,dock_interrupt,
               IRQF_TRIGGER_RISING|IRQF_TRIGGER_FALLING|IRQF_ONESHOT, "dock-irq", context);
        if (err < 0) {
            pr_err("Dock driver request_irq fail err = %d\n", err);
            goto free_gpio;
        }
        INIT_DELAYED_WORK(&context->work, dock_report_work);
        context->event_wq = create_workqueue("dock_irq_event_wq");
    }else{
        pr_err("dock driver irq gpio not provided\n");
        goto free_context;
    }

    context->dock_class = class_create(THIS_MODULE, "dock");
    if (IS_ERR(context->dock_class)) {
        pr_err("Dock driver failed to create class \n");
        ret = PTR_ERR(context->dock_class);
        goto free_work_source;
    }

    context->dev = device_create(context->dock_class, NULL,MKDEV(0, 0), NULL, "dock2");
    if (IS_ERR(context->dev)) {
        pr_err("Dock driver failed to create device \n");
        ret = PTR_ERR(context->dev);
        goto destroy_class;
    }

    if (device_create_file(context->dev, &dev_attr_trigger))
        pr_err("Dock driver create device trigger file failed");

    context->force_update = 0;
    platform_set_drvdata(pdev, context);
    dock_context_info = context;

    return 0;

destroy_class:
    class_destroy(context->dock_class);
free_work_source:
    destroy_workqueue(context->event_wq);
free_gpio:
    gpio_free(context->irq_gpio);
free_context:
    platform_set_drvdata(pdev, NULL);
    kfree(context);

    ret = err;
    return ret;
}

static int dock_detect_remove(struct platform_device *pdev)
{
    struct dock_context *context = platform_get_drvdata(pdev);
    class_destroy(context->dock_class);
    destroy_workqueue(context->event_wq);
    gpio_free(context->irq_gpio);
    platform_set_drvdata(pdev, NULL);
    kfree(context);
    return 0;
}

static const struct of_device_id dock_detect_dt_match[] = {
	{ .compatible = "pogo_detect" },
	{},
};

static struct platform_driver dock_detect_driver = {
    .probe = dock_detect_probe,
    .remove = dock_detect_remove,
    .driver = {
        .name = "pogo_detect",
        .owner = THIS_MODULE,
        .pm = NULL,
        .of_match_table = dock_detect_dt_match,
    },
};

static int __init dock_detect_init(void)
{
    pr_err("dock driver init enter\n");
    return platform_driver_register(&dock_detect_driver);
}

static void __exit dock_detect_exit(void)
{
    pr_err("dock driver uninit enter\n");
    platform_driver_unregister(&dock_detect_driver);
}

module_init(dock_detect_init);
module_exit(dock_detect_exit);

MODULE_DESCRIPTION("Dock detect driver");
MODULE_LICENSE("GPL v2");
MODULE_AUTHOR("Longcheer HuiZhou Team");
