/*
 * Raspberry Pi emulation (c) 2012 Gregory Estrade
 * Upstreaming code cleanup [including bcm2835_*] (c) 2013 Jan Petrous
 *
 * Rasperry Pi 2 emulation and refactoring Copyright (c) 2015, Microsoft
 * Written by Andrew Baumann
 *
 * This code is licensed under the GNU GPLv2 and later.
 */

#include "hw/arm/bcm2836.h"
#include "hw/arm/raspi_platform.h"
#include "hw/sysbus.h"
#include "sysemu/sysemu.h" /* for smp_cpus */
#include "exec/address-spaces.h"

#define DEFAULT_VCRAM_SIZE 0x4000000

static void bcm2836_init(Object *obj)
{
    BCM2836State *s = BCM2836(obj);
    SysBusDevice *dev;
    int n;

    /* TODO: probably shouldn't be using smp_cpus here */
    assert(smp_cpus <= BCM2836_NCPUS);
    for (n = 0; n < smp_cpus; n++) {
        object_initialize(&s->cpus[n], sizeof(s->cpus[n]),
                          "cortex-a15-" TYPE_ARM_CPU);
        object_property_add_child(obj, "cpu[*]", OBJECT(&s->cpus[n]),
                                  &error_abort);
    }

    s->ic = dev = SYS_BUS_DEVICE(object_new("bcm2836_control"));
    object_property_add_child(obj, "ic", OBJECT(dev), NULL);
    qdev_set_parent_bus(DEVICE(dev), sysbus_get_default());

    object_initialize(&s->peripherals, sizeof(s->peripherals),
                      TYPE_BCM2835_PERIPHERALS);
    object_property_add_child(obj, "peripherals", OBJECT(&s->peripherals),
                              &error_abort);
    qdev_set_parent_bus(DEVICE(&s->peripherals), sysbus_get_default());
}

static void bcm2836_realize(DeviceState *dev, Error **errp)
{
    BCM2836State *s = BCM2836(dev);
    Error *err = NULL;
    int n;

    /* common peripherals from bcm2835 */
    object_property_set_bool(OBJECT(&s->peripherals), true, "realized", &err);
    if (err) {
        error_propagate(errp, err);
        return;
    }

    sysbus_mmio_map_overlap(SYS_BUS_DEVICE(&s->peripherals), 0,
                            BCM2836_PERI_BASE, 1);

    /* bcm2836 interrupt controller (and mailboxes, etc.) */
    object_property_set_bool(OBJECT(s->ic), true, "realized", &err);
    if (err) {
        error_propagate(errp, err);
        return;
    }

    sysbus_mmio_map(SYS_BUS_DEVICE(s->ic), 0, BCM2836_CONTROL_BASE);

    sysbus_connect_irq(SYS_BUS_DEVICE(&s->peripherals), 0,
                       qdev_get_gpio_in_named(DEVICE(s->ic), "gpu_irq", 0));
    sysbus_connect_irq(SYS_BUS_DEVICE(&s->peripherals), 1,
                       qdev_get_gpio_in_named(DEVICE(s->ic), "gpu_fiq", 0));

    /* TODO: probably shouldn't be using smp_cpus here */
    assert(smp_cpus <= BCM2836_NCPUS);
    for (n = 0; n < smp_cpus; n++) {
        /* Mirror bcm2836, which has clusterid set to 0xf */
        s->cpus[n].mp_affinity = 0xF00 | n;

        /* set periphbase/CBAR value for CPU-local registers */
        object_property_set_int(OBJECT(&s->cpus[n]),
                                BCM2836_PERI_BASE + MCORE_OFFSET,
                                "reset-cbar", &err);
        if (err) {
            error_report_err(err);
            exit(1);
        }

        object_property_set_bool(OBJECT(&s->cpus[n]), true, "realized", &err);
        if (err) {
            error_report_err(err);
            exit(1);
        }

        /* Connect irq/fiq outputs from the interrupt controller. */
        qdev_connect_gpio_out_named(DEVICE(s->ic), "irq", n,
                                    qdev_get_gpio_in(DEVICE(&s->cpus[n]),
                                                     ARM_CPU_IRQ));
        qdev_connect_gpio_out_named(DEVICE(s->ic), "fiq", n,
                                    qdev_get_gpio_in(DEVICE(&s->cpus[n]),
                                                     ARM_CPU_FIQ));

        /* Connect timers from the CPU to the interrupt controller */
        s->cpus[n].gt_timer_outputs[GTIMER_PHYS]
            = qdev_get_gpio_in_named(DEVICE(s->ic), "cntpsirq", 0);
        s->cpus[n].gt_timer_outputs[GTIMER_VIRT]
            = qdev_get_gpio_in_named(DEVICE(s->ic), "cntvirq", 0);
    }
}

static Property bcm2836_props[] = {
    DEFINE_PROP_SIZE("vcram-size", BCM2836State, vcram_size, DEFAULT_VCRAM_SIZE),
    DEFINE_PROP_END_OF_LIST()
};

static void bcm2836_class_init(ObjectClass *oc, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);

    dc->props = bcm2836_props;
    dc->realize = bcm2836_realize;

    /*
     * Reason: creates an ARM CPU, thus use after free(), see
     * arm_cpu_class_init()
     */
    dc->cannot_destroy_with_object_finalize_yet = true;
}

static const TypeInfo bcm2836_type_info = {
    .name = TYPE_BCM2836,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(BCM2836State),
    .instance_init = bcm2836_init,
    .class_init = bcm2836_class_init,
};

static void bcm2836_register_types(void)
{
    type_register_static(&bcm2836_type_info);
}

type_init(bcm2836_register_types)
