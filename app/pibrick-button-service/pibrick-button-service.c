// SPDX-License-Identifier: GPL-3.0-or-later
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <signal.h>
#include <poll.h>
#include <time.h>
#include <sys/ioctl.h>

#include <linux/input.h>
#include <linux/uinput.h>
#include <linux/gpio.h>


/*
 * piBrick Button Service
 *
 * GPIO20 / gpiochip10 = POWER
 * GPIO23 / gpiochip0  = USER
 *
 * Both buttons are active-low.
 *
 *
 * POWER:
 *
 *   Press
 *       |
 *       +-- held < POWER_LONG_PRESS_MS
 *       |       |
 *       |       +-- release -> power-short.sh
 *       |
 *       +-- held >= POWER_LONG_PRESS_MS
 *               |
 *               +-- immediately -> power-long.sh
 *
 *
 * USER:
 *
 *   Press
 *       |
 *       +-- held < USER_LONG_PRESS_MS
 *       |       |
 *       |       +-- release -> user-short.sh
 *       |
 *       +-- held >= USER_LONG_PRESS_MS
 *               |
 *               +-- immediately -> user-long.sh
 *
 *
 * USER has priority over POWER:
 *
 *   GPIO23 active
 *       -> USER pressed
 *
 *   GPIO20 active + GPIO23 active
 *       -> USER
 *       -> POWER ignored
 *
 *   If POWER is released while USER is still physically active:
 *
 *       POWER release
 *           ->
 *       logical USER release
 *
 *   The later physical USER release is ignored.
 */


/* --------------------------------------------------------------------------
 * GPIO configuration
 * -------------------------------------------------------------------------- */

#define GPIOCHIP_POWER              "/dev/gpiochip10"
#define GPIO_POWER_LINE             20

#define GPIOCHIP_USER               "/dev/gpiochip0"
#define GPIO_USER_LINE              23


/* --------------------------------------------------------------------------
 * Button timing
 *
 * Change these values to configure long-press detection.
 * -------------------------------------------------------------------------- */

#define POWER_LONG_PRESS_MS         700
#define USER_LONG_PRESS_MS          700


/* --------------------------------------------------------------------------
 * Globals
 * -------------------------------------------------------------------------- */

static int gpio_power_fd = -1;
static int gpio_user_fd = -1;
static int uk_fd=-1;
static volatile sig_atomic_t running = 1;


/*
 * Physical GPIO state.
 *
 * Active-low:
 *
 *   0 = active / pressed
 *   1 = inactive / released
 */
static int power_active = 0;
static int user_active = 0;


/*
 * POWER logical state.
 */
static int power_candidate = 0;
static int power_suppressed = 0;
static int power_long_triggered = 0;

static long long power_press_time = 0;


/*
 * USER logical state.
 */
static int user_logical_active = 0;
static int user_long_triggered = 0;

static long long user_press_time = 0;


/* --------------------------------------------------------------------------
 * Time
 * -------------------------------------------------------------------------- */

static long long monotonic_ms(void)
{
    struct timespec ts;

    if (clock_gettime(CLOCK_MONOTONIC, &ts) < 0) {
        return 0;
    }

    return ((long long)ts.tv_sec * 1000LL) +
           ((long long)ts.tv_nsec / 1000000LL);
}


/* --------------------------------------------------------------------------
 * Init uInput
 * -------------------------------------------------------------------------- */
int uk_init(){
    uk_fd = open("/dev/uinput", O_WRONLY | O_NONBLOCK);
    if(uk_fd < 0){
        printf("Error opening /dev/uinput: %s\n", strerror(errno));
        return -1;
    }
    if(ioctl(uk_fd, UI_SET_EVBIT, EV_KEY) < 0){
        printf("Error setting EV_KEY: %s\n", strerror(errno));
        return -1;
    }
    printf("UI_SET_EVBIT OK\n");
    if(ioctl(uk_fd, UI_SET_KEYBIT, KEY_POWER) < 0){
        printf("Error setting KEY_POWER: %s\n", strerror(errno));
        return -1;
    }
    printf("UI_SET_KEYBIT OK\n");
    struct uinput_user_dev uidev;
    memset(&uidev, 0, sizeof(uidev));
    snprintf(uidev.name, UINPUT_MAX_NAME_SIZE, "pibrickbtn");
    uidev.id.bustype = BUS_USB;
    uidev.id.vendor  = 0x1234;
    uidev.id.product = 0x5678;
    uidev.id.version = 1;
    if(write(uk_fd, &uidev, sizeof(uidev)) < 0){
        printf("Error writing uinput_user_dev: %s\n", strerror(errno));
        return -1;
    }
    printf("uinput_user_dev OK\n");
    if(ioctl(uk_fd, UI_DEV_CREATE) < 0){
        printf("Error creating uinput device: %s\n", strerror(errno));
        return -1;
    }
    printf("UI_DEV_CREATE OK\n");
    
    return 0;
}


/* --------------------------------------------------------------------------
 * Close uInput
 * -------------------------------------------------------------------------- */
void uk_close(){
    if (uk_fd>=0){
        close(uk_fd);
        uk_fd=-1;
    }
}

/* --------------------------------------------------------------------------
 * Send Key
 * -------------------------------------------------------------------------- */
int uk_send_key(int keycode, int keystate){
    if(uk_fd < 0){
        return -1;
    }
    struct input_event ev;
    memset(&ev, 0, sizeof(struct input_event));
    ev.type = EV_KEY;
    ev.code = keycode;
    ev.value = keystate;
    if(write(uk_fd, &ev, sizeof(struct input_event)) < 0){
        return -1;
    }
    memset(&ev, 0, sizeof(struct input_event));
    ev.type = EV_SYN;
    ev.code = 0;
    ev.value = 0;
    if(write(uk_fd, &ev, sizeof(struct input_event)) < 0){
        return -1;
    }
    return 0;
}

/* --------------------------------------------------------------------------
 * Signal handling
 * -------------------------------------------------------------------------- */

static void signal_handler(int sig)
{
    (void)sig;

    running = 0;
}


/* --------------------------------------------------------------------------
 * GPIO helpers
 * -------------------------------------------------------------------------- */

static int gpio_request_input(
    const char *chip_path,
    unsigned int line,
    unsigned int flags)
{
    int chip_fd;
    struct gpio_v2_line_request req;

    memset(&req, 0, sizeof(req));

    chip_fd = open(
        chip_path,
        O_RDWR | O_CLOEXEC
    );

    if (chip_fd < 0) {
        fprintf(
            stderr,
            "Failed to open %s: %s\n",
            chip_path,
            strerror(errno)
        );

        return -1;
    }

    req.offsets[0] = line;
    req.num_lines = 1;
    req.config.flags = flags;

    req.event_buffer_size = 16;

    snprintf(
        req.consumer,
        sizeof(req.consumer),
        "pibrick-button-service"
    );

    if (ioctl(
            chip_fd,
            GPIO_V2_GET_LINE_IOCTL,
            &req) < 0) {

        fprintf(
            stderr,
            "Failed to request %s GPIO%u: %s\n",
            chip_path,
            line,
            strerror(errno)
        );

        close(chip_fd);

        return -1;
    }

    close(chip_fd);

    return req.fd;
}


static int gpio_button_init(
    const char *chip,
    unsigned int line,
    const char *name)
{
    unsigned int flags =
        GPIO_V2_LINE_FLAG_INPUT |
        GPIO_V2_LINE_FLAG_BIAS_PULL_UP |
        GPIO_V2_LINE_FLAG_EDGE_FALLING |
        GPIO_V2_LINE_FLAG_EDGE_RISING;

    int fd = gpio_request_input(
        chip,
        line,
        flags
    );

    if (fd < 0) {
        return -1;
    }

    printf(
        "%s initialized: %s GPIO%u fd=%d\n",
        name,
        chip,
        line,
        fd
    );

    return fd;
}


/*
 * Read one GPIO event.
 *
 * Returns:
 *
 *   0  = falling edge
 *   1  = rising edge
 *  -2  = interrupted
 *  -1  = error
 */
static int gpio_read_event(int fd)
{
    struct gpio_v2_line_event event;

    memset(
        &event,
        0,
        sizeof(event)
    );

    ssize_t n = read(
        fd,
        &event,
        sizeof(event)
    );

    if (n < 0) {

        if (errno == EINTR) {
            return -2;
        }

        fprintf(
            stderr,
            "Failed to read GPIO event: %s\n",
            strerror(errno)
        );

        return -1;
    }

    if (n != sizeof(event)) {

        fprintf(
            stderr,
            "Invalid GPIO event size: %zd\n",
            n
        );

        return -1;
    }

    if (event.id == GPIO_V2_LINE_EVENT_FALLING_EDGE) {
        return 0;
    }

    if (event.id == GPIO_V2_LINE_EVENT_RISING_EDGE) {
        return 1;
    }

    return -1;
}


/*
 * Read current GPIO value.
 *
 * Returns:
 *
 *   0 = active
 *   1 = inactive
 */
static int gpio_get_value(int fd)
{
    struct gpio_v2_line_values values;

    memset(
        &values,
        0,
        sizeof(values)
    );

    values.mask = 1ULL;

    if (ioctl(
            fd,
            GPIO_V2_LINE_GET_VALUES_IOCTL,
            &values) < 0) {

        fprintf(
            stderr,
            "Failed to read GPIO value: %s\n",
            strerror(errno)
        );

        return -1;
    }

    return (values.bits & 1ULL) ? 1 : 0;
}


static void gpio_close(void)
{
    if (gpio_power_fd >= 0) {
        close(gpio_power_fd);
        gpio_power_fd = -1;
    }

    if (gpio_user_fd >= 0) {
        close(gpio_user_fd);
        gpio_user_fd = -1;
    }
}


/* --------------------------------------------------------------------------
 * POWER actions
 * -------------------------------------------------------------------------- */

static void power_long_press(void)
{
    if (power_long_triggered) {
        return;
    }

    power_long_triggered = 1;

    printf("[TRIGGER] POWER LONG PRESS\n");

    uk_send_key(KEY_POWER, 1);
    uk_send_key(KEY_POWER, 0);
}


static void power_short_press(void)
{
    printf("[TRIGGER] POWER SHORT PRESS\n");

    system(
        "bash /etc/pibrick/power-short.sh"
    );
}


/* --------------------------------------------------------------------------
 * USER actions
 * -------------------------------------------------------------------------- */

static void user_long_press(void)
{
    if (user_long_triggered) {
        return;
    }

    user_long_triggered = 1;

    printf("[TRIGGER] USER LONG PRESS\n");

    system(
        "bash /etc/pibrick/user-long.sh"
    );
}


static void user_short_press(void)
{
    printf("[TRIGGER] USER SHORT PRESS\n");

    system(
        "bash /etc/pibrick/user-short.sh"
    );
}


/* --------------------------------------------------------------------------
 * USER logical release
 * -------------------------------------------------------------------------- */

/*
 * Generate the logical USER release.
 *
 * This may be caused by:
 *
 *   1. Physical GPIO23 release
 *   2. GPIO20 release while GPIO23 is still physically active
 */
static void user_logical_release(void)
{
    if (!user_logical_active) {
        return;
    }

    user_logical_active = 0;

    /*
     * If long press has already fired, do not fire short press.
     */
    if (user_long_triggered) {

        // printf("USER RELEASE AFTER LONG PRESS\n");
        user_long_triggered = 0;

        return;
    }

    /*
     * Long press did not happen, therefore this was a short press.
     */
    user_short_press();

    user_long_triggered = 0;
}


/* --------------------------------------------------------------------------
 * POWER handling
 * -------------------------------------------------------------------------- */

static void power_press(void)
{
    /*
     * USER has priority.
     *
     * POWER must not become a candidate while USER
     * is physically active.
     */
    if (user_active) {

        // printf("POWER PRESS IGNORED: USER ACTIVE\n");

        power_candidate = 0;
        power_suppressed = 1;
        power_long_triggered = 0;

        return;
    }

    power_candidate = 1;
    power_suppressed = 0;
    power_long_triggered = 0;

    power_press_time = monotonic_ms();

    // printf("> POWER PRESS\n");
}


static void power_release(void)
{
    /*
     * POWER wasn't a valid candidate.
     *
     * It may have been suppressed because USER was active.
     */
    if (!power_candidate) {

        if (power_suppressed && user_active) {

            // printf("POWER RELEASE -> USER RELEASE\n");

            /*
             * Convert this physical POWER release into
             * the logical USER release.
             */
            user_logical_release();

            /*
             * USER is still physically down, but its
             * logical press has ended.
             */
            user_active = 0;

            power_suppressed = 0;

            return;
        }

        power_suppressed = 0;

        return;
    }

    power_candidate = 0;

    /*
     * USER took priority while POWER was held.
     *
     * POWER release becomes USER release.
     */
    if (power_suppressed || user_active) {

        // printf("POWER RELEASE -> USER RELEASE\n");

        power_suppressed = 0;

        user_logical_release();

        /*
         * The physical USER button may still be held,
         * but the logical USER interaction has ended.
         */
        user_active = 0;

        return;
    }

    /*
     * Normal POWER release.
     *
     * If long press already fired, do nothing.
     *
     * Otherwise this was a short press.
     */
    if (power_long_triggered) {

        // printf("POWER RELEASE AFTER LONG PRESS\n");

        power_long_triggered = 0;

        return;
    }

    power_short_press();
}


/* --------------------------------------------------------------------------
 * USER handling
 * -------------------------------------------------------------------------- */

static void user_press(void)
{
    /*
     * Ignore duplicate logical presses.
     */
    if (user_logical_active) {
        return;
    }

    user_logical_active = 1;
    user_long_triggered = 0;

    user_press_time = monotonic_ms();

    // printf("USER PRESS\n");

    /*
     * USER has priority over POWER.
     *
     * If POWER is already being held, suppress it.
     */
    if (power_candidate) {

        // printf("USER PRESS: POWER SUPPRESSED\n");

        power_suppressed = 1;
    }
}


static void user_release(void)
{
    /*
     * The logical USER button may already have been
     * released by a POWER release.
     */
    if (!user_logical_active) {
        return;
    }

    user_logical_release();
}


/* --------------------------------------------------------------------------
 * Event processing
 * -------------------------------------------------------------------------- */

static void process_power_event(int event)
{
    if (event == 0) {

        /*
         * GPIO20 falling = POWER pressed.
         */
        power_active = 1;

        power_press();

    } else if (event == 1) {

        /*
         * GPIO20 rising = POWER released.
         */
        power_active = 0;

        power_release();
    }
}


static void process_user_event(int event)
{
    if (event == 0) {

        /*
         * GPIO23 falling = USER pressed.
         */
        user_active = 1;

        user_press();

    } else if (event == 1) {

        /*
         * GPIO23 rising = USER released.
         */
        user_active = 0;

        user_release();
    }
}


/* --------------------------------------------------------------------------
 * Long press timer processing
 * -------------------------------------------------------------------------- */

/*
 * Check whether either button has reached its long-press
 * threshold.
 *
 * This is called whenever poll() times out or wakes up.
 */
static void process_long_press(void)
{
    long long now = monotonic_ms();

    /*
     * POWER long press.
     *
     * Only a valid POWER candidate can trigger it.
     */
    if (power_candidate &&
        !power_suppressed &&
        !power_long_triggered) {

        long long elapsed =
            now - power_press_time;

        if (elapsed >= POWER_LONG_PRESS_MS) {

            power_long_press();
        }
    }

    /*
     * USER long press.
     */
    if (user_logical_active &&
        !user_long_triggered) {

        long long elapsed =
            now - user_press_time;

        if (elapsed >= USER_LONG_PRESS_MS) {

            user_long_press();
        }
    }
}


/*
 * Calculate the time until the next long-press deadline.
 *
 * Returns:
 *
 *   -1 = no active timer
 *   >=0 = milliseconds until deadline
 */
static int get_poll_timeout(void)
{
    long long now = monotonic_ms();

    long long next_deadline = -1;

    /*
     * POWER deadline.
     */
    if (power_candidate &&
        !power_suppressed &&
        !power_long_triggered) {

        long long deadline =
            power_press_time +
            POWER_LONG_PRESS_MS;

        next_deadline = deadline;
    }

    /*
     * USER deadline.
     */
    if (user_logical_active &&
        !user_long_triggered) {

        long long deadline =
            user_press_time +
            USER_LONG_PRESS_MS;

        if (next_deadline < 0 ||
            deadline < next_deadline) {

            next_deadline = deadline;
        }
    }

    if (next_deadline < 0) {
        return -1;
    }

    long long remaining =
        next_deadline - now;

    if (remaining <= 0) {
        return 0;
    }

    if (remaining > 2147483647LL) {
        return 2147483647;
    }

    return (int)remaining;
}


/* --------------------------------------------------------------------------
 * Main event loop
 * -------------------------------------------------------------------------- */

static void monitor_buttons(void)
{
    struct pollfd fds[2];

    while (running) {

        memset(
            fds,
            0,
            sizeof(fds)
        );

        /*
         * GPIO20 = POWER.
         */
        fds[0].fd = gpio_power_fd;
        fds[0].events = POLLIN;

        /*
         * GPIO23 = USER.
         */
        fds[1].fd = gpio_user_fd;
        fds[1].events = POLLIN;

        /*
         * Wake either when:
         *
         *   - a GPIO changes
         *   - the next long-press deadline arrives
         */
        int timeout = get_poll_timeout();

        int rc = poll(
            fds,
            2,
            timeout
        );

        if (rc < 0) {

            if (errno == EINTR) {
                continue;
            }

            fprintf(
                stderr,
                "Button poll failed: %s\n",
                strerror(errno)
            );

            break;
        }

        /*
         * First process GPIO events.
         */
        if (fds[0].revents & POLLIN) {

            int event =
                gpio_read_event(
                    gpio_power_fd
                );

            if (event >= 0) {
                process_power_event(event);
            }
        }

        if (fds[1].revents & POLLIN) {

            int event =
                gpio_read_event(
                    gpio_user_fd
                );

            if (event >= 0) {
                process_user_event(event);
            }
        }

        /*
         * Then process any long-press deadline.
         *
         * This is what makes long press trigger while
         * the button is still physically held.
         */
        process_long_press();
    }
}


/* --------------------------------------------------------------------------
 * Main
 * -------------------------------------------------------------------------- */

int main(void)
{
    setvbuf(
        stdout,
        NULL,
        _IONBF,
        0
    );

    signal(
        SIGINT,
        signal_handler
    );

    signal(
        SIGTERM,
        signal_handler
    );


    /*
     * Keep the existing piBrick GPIO ownership behavior.
     */
    system("rmmod gpio_keys");
    // system("rmmod hyn_ts");
    // system("modprobe hyn_ts");
    uk_init();


    /*
     * GPIO20 = POWER.
     */
    gpio_power_fd = gpio_button_init(
        GPIOCHIP_POWER,
        GPIO_POWER_LINE,
        "POWER button"
    );

    if (gpio_power_fd < 0) {
        return 1;
    }


    /*
     * GPIO23 = USER.
     */
    gpio_user_fd = gpio_button_init(
        GPIOCHIP_USER,
        GPIO_USER_LINE,
        "USER button"
    );

    if (gpio_user_fd < 0) {

        gpio_close();

        return 1;
    }


    /*
     * Read initial physical states.
     *
     * Active-low:
     *
     *   0 = pressed
     *   1 = released
     */
    int power_value =
        gpio_get_value(gpio_power_fd);

    int user_value =
        gpio_get_value(gpio_user_fd);

    if (power_value >= 0) {
        power_active = (power_value == 0);
    }

    if (user_value >= 0) {
        user_active = (user_value == 0);
    }


    /*
     * If USER is physically held when the service starts,
     * consider it logically active.
     */
    if (user_active) {

        user_logical_active = 1;
        user_press_time = monotonic_ms();

        printf(
            "USER is already active at startup\n"
        );
    }


    printf("\n");
    printf("piBrick button service started\n");

    printf(
        "  POWER: %s GPIO%d\n",
        GPIOCHIP_POWER,
        GPIO_POWER_LINE
    );

    printf(
        "  USER : %s GPIO%d\n",
        GPIOCHIP_USER,
        GPIO_USER_LINE
    );

    printf(
        "  POWER long press: %d ms\n",
        POWER_LONG_PRESS_MS
    );

    printf(
        "  USER long press: %d ms\n",
        USER_LONG_PRESS_MS
    );

    printf(
        "  Initial state: POWER=%s USER=%s\n",
        power_active ? "ACTIVE" : "INACTIVE",
        user_active ? "ACTIVE" : "INACTIVE"
    );

    printf("\n");


    /*
     * Monitor both GPIOs.
     */
    monitor_buttons();


    printf(
        "Stopping piBrick button service\n"
    );

    gpio_close();

    uk_close();

    return 0;
}

