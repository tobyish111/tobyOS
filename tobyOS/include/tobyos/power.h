/* power.h -- Milestone 8 ACPI power management.
 *
 * Sleep states, CPU frequency scaling, battery monitoring, backlight,
 * and idle/lid policy.
 */

#ifndef TOBYOS_POWER_H
#define TOBYOS_POWER_H

#include <tobyos/types.h>
#include <stdint.h>

enum power_state {
    POWER_S0_ON,
    POWER_S3_SLEEP,
    POWER_S4_HIBERNATE,
    POWER_S5_OFF
};

enum power_plan {
    PLAN_BALANCED,
    PLAN_PERFORMANCE,
    PLAN_SAVER
};

struct battery_info {
    int present;
    int charging;
    int percent;
    int minutes_remaining;
    int voltage_mv;
    int current_ma;
};

/* Core power management */
void power_init(void);
int  power_suspend(void);
int  power_hibernate(void);
void power_shutdown(void);
void power_reboot(void);

/* CPU frequency scaling */
void     cpufreq_init(void);
void     cpufreq_set_governor(enum power_plan plan);
uint32_t cpufreq_get_mhz(void);

/* Battery */
int battery_get_info(struct battery_info *out);

/* Backlight */
void backlight_set(int percent);
int  backlight_get(void);

/* Idle and lid management */
void power_set_idle_timeout(int seconds_dim, int seconds_off, int seconds_sleep);
void power_handle_lid(int closed);

#endif /* TOBYOS_POWER_H */
