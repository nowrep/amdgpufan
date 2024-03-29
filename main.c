#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <signal.h>
#include <unistd.h>
#include <stdlib.h>
#include <dirent.h>
#include <stdbool.h>
#include <inttypes.h>

struct fan_curve {
    uint8_t temp;
    uint8_t pwm;
};

static struct {
    char *card;
    struct fan_curve fan_curve[20];
    int fan_curve_count;
} config;

struct gpu_temperatures {
    uint16_t temperature_edge;
    uint16_t temperature_hotspot;
    uint16_t temperature_mem;
};

static char gpu_temp1_file[128];
static char gpu_temp2_file[128];
static char gpu_temp3_file[128];
static char pwm_enable_file[128];
static char pwm_file[128];

static bool debug = false;
static bool no_memory = false;
static bool closing = false;
static int16_t current_pwm = -1;
static int slowdown_ticks = 0;

#define DBG(x, ...) if (debug) { fprintf(stderr, "D: " x "\n", __VA_ARGS__); }

static bool read_temp_input(const char *file, uint16_t *out)
{
    FILE *f = fopen(file, "r");
    if (!f) {
        return false;
    }
    char buf[32];
    if (fread(buf, 1, sizeof(buf), f) <= 0) {
        fclose(f);
        return false;
    }
    fclose(f);
    *out = atoi(buf) / 1000;
    return *out != 0;
}

struct gpu_temperatures *read_temperatures()
{
    static struct gpu_temperatures temperatures;
    if (!read_temp_input(gpu_temp1_file, &temperatures.temperature_edge)) {
        return NULL;
    }
    read_temp_input(gpu_temp2_file, &temperatures.temperature_hotspot);
    if (!no_memory) {
        read_temp_input(gpu_temp3_file, &temperatures.temperature_mem);
    }
    return &temperatures;
}

static uint8_t read_curve_pwm(uint8_t temp)
{
    if (temp <= config.fan_curve[0].temp) {
        return config.fan_curve[0].pwm;
    } else if (temp >= config.fan_curve[config.fan_curve_count - 1].temp) {
        return config.fan_curve[config.fan_curve_count - 1].pwm;
    }
    for (int i = 1; i < config.fan_curve_count; ++i) {
        if (temp > config.fan_curve[i].temp) {
            continue;
        }
        struct fan_curve lo = config.fan_curve[i - 1];
        struct fan_curve hi = config.fan_curve[i];
        return (temp - lo.temp) * (hi.pwm - lo.pwm) / (hi.temp - lo.temp) + lo.pwm;
    }
    return 255;
}

static bool write_file(const char *file, const char *buffer, size_t size)
{
    FILE *f = fopen(file, "w");
    if (!f) {
        return false;
    }
    if (fwrite(buffer, 1, size, f) != size) {
        fclose(f);
        return false;
    }
    fclose(f);
    return true;
}

static bool set_pwm_manual(bool manual)
{
    char c = manual ? '1' : '2';
    return write_file(pwm_enable_file, &c, 1);
}

static bool set_pwm(uint8_t value)
{
    char buf[4];
    uint8_t val = value / 100.0 * 255;
    snprintf(buf, sizeof(buf), "%" PRIu8, val);
    return write_file(pwm_file, buf, 3);
}

static bool load_config(const char *path)
{
    config.card = NULL;
    config.fan_curve_count = 0;

    FILE *f = fopen(path, "r");
    if (!f) {
        return false;
    }
    char *line = NULL;
    size_t len = 0;
    ssize_t s;
    while ((s = getline(&line, &len, f))) {
        if (s < 0) {
            break;
        }
        if (s == 0 || line[0] == '#') {
            continue;
        }
        line[strlen(line) - 1] = '\0';
        if (!config.card) {
            config.card = strdup(line);
        } else if (!strcmp(line, "no-memory")) {
            no_memory = true;
        } else {
            unsigned temp, pwm;
            sscanf(line, "%u %u", &temp, &pwm);
            if (config.fan_curve_count >= 20) {
                fprintf(stderr, "Maximum 20 fan curve entries reached\n");
                return false;
            }
            if (pwm > 100) {
                fprintf(stderr, "Maximum fan speed value is 100\n");
                return false;
            }
            struct fan_curve *curve = &config.fan_curve[config.fan_curve_count++];
            curve->temp = temp;
            curve->pwm = pwm;
        }
    }
    free(line);

    if (config.fan_curve_count < 2) {
        fprintf(stderr, "At least 2 fan curve points required\n");
        return false;
    }
    for (int i = 1; i < config.fan_curve_count; ++i) {
        struct fan_curve lo = config.fan_curve[i - 1];
        struct fan_curve hi = config.fan_curve[i];
        if (lo.temp > hi.temp || lo.pwm > hi.pwm) {
            fprintf(stderr, "Fan curve needs to be monotonic (increasing)\n");
            return false;
        }
    }

    DBG("Using card '%s' with %d fan curve entries", config.card, config.fan_curve_count);

    return true;
}

static int get_vendor(const char *card)
{
    char buf[512];
    snprintf(buf, sizeof(buf), "/sys/class/drm/%s/device/vendor", card);

    FILE *f = fopen(buf, "r");
    if (!f) {
        return 0;
    }

    size_t s = fread(buf, 1, sizeof(buf), f);
    fclose(f);

    if (s <= 0) {
        return 0;
    }

    buf[s] = '\0';
    return strtol(buf, NULL, 16);
}

static bool load_paths(const char *card)
{
    char buf[512];
    snprintf(buf, sizeof(buf), "/sys/class/drm/%s/device/hwmon", card);

    DIR *d = opendir(buf);
    if (!d) {
        return false;
    }
    const char *hwmon = NULL;
    struct dirent *dir;
    while ((dir = readdir(d))) {
        if (!strncmp(dir->d_name, "hwmon", 5)) {
            hwmon = dir->d_name;
            break;
        }
    }
    closedir(d);

    snprintf(buf, sizeof(buf), "/sys/class/drm/%s/device/hwmon/%s/pwm1_enable", card, hwmon);
    strcpy(pwm_enable_file, buf);

    snprintf(buf, sizeof(buf), "/sys/class/drm/%s/device/hwmon/%s/pwm1", card, hwmon);
    strcpy(pwm_file, buf);

    snprintf(buf, sizeof(buf), "/sys/class/drm/%s/device/hwmon/%s/temp1_input", card, hwmon);
    strcpy(gpu_temp1_file, buf);

    snprintf(buf, sizeof(buf), "/sys/class/drm/%s/device/hwmon/%s/temp2_input", card, hwmon);
    strcpy(gpu_temp2_file, buf);

    snprintf(buf, sizeof(buf), "/sys/class/drm/%s/device/hwmon/%s/temp3_input", card, hwmon);
    strcpy(gpu_temp3_file, buf);

    return !access(gpu_temp1_file, R_OK) && !access(pwm_enable_file, W_OK) && !access(pwm_file, W_OK);
}

static void print_help()
{
    printf("Usage: amdgpufan [options]\n");
    printf("\n");
    printf("Options:\n");
    printf("  -c CONFIG_FILE        Specify config path\n");
    printf("  -d --debug            Enable debug logging\n");
    printf("  -h --help             Show this help message\n");
    printf("  -v --version          Show version\n");
}

static void print_version()
{
    printf("%s\n", AMDGPUFAN_VERSION);
}

static void sig_handler(int sig)
{
    (void)sig;
    closing = true;
}

int main(int argc, char *argv[])
{
    struct sigaction sig;
    sig.sa_handler = sig_handler;
    sigemptyset(&sig.sa_mask);
    sig.sa_flags = 0;
    sigaction(SIGINT, &sig, NULL);
    sigaction(SIGTERM, &sig, NULL);

    const char *conf_file = "/etc/amdgpufan.conf";

    if (argc > 1) {
        if (!strcmp(argv[1], "-h") || !strcmp(argv[1], "--help")) {
            print_help();
            return 0;
        } else if (!strcmp(argv[1], "-v") || !strcmp(argv[1], "--version")) {
            print_version();
            return 0;
        }
        while (argc > 1) {
            if (!strcmp(argv[1], "-c")) {
                if (argc < 3) {
                    print_help();
                    return 1;
                }
                conf_file = argv[2];
                argc -= 2;
                argv += 2;
            } else if (!strcmp(argv[1], "-d") || !strcmp(argv[1], "--debug")) {
                debug = true;
                argc -= 1;
                argv += 1;
            }
        }
    }

    if (!load_config(conf_file)) {
        fprintf(stderr, "Failed to load config from '%s'\n", conf_file);
        return 1;
    }

    int vendor = get_vendor(config.card);
    if (vendor != 0x1002) {
        fprintf(stderr, "Warning: Card vendor not AMD: %#x\n", vendor);
    }

    if (!load_paths(config.card)) {
        fprintf(stderr, "Failed to access sysfs for card '%s'\n", config.card);
        return 2;
    }

    set_pwm_manual(true);

    while (!closing) {
        sleep(1);
        struct gpu_temperatures *temperatures = read_temperatures();
        if (!temperatures) {
            fprintf(stderr, "Failed to read temperatures\n");
            break;
        }

        uint8_t temp = temperatures->temperature_edge;
        if (temperatures->temperature_hotspot > temp) {
            temp = temperatures->temperature_hotspot;
        }
        if (temperatures->temperature_mem > temp) {
            temp = temperatures->temperature_mem;
        }
        if (temp == 0) {
            fprintf(stderr, "Invalid temperature read\n");
            break;
        }

        uint8_t pwm = read_curve_pwm(temp);
        if (pwm == 255) {
            fprintf(stderr, "Invalid pwm calculated\n");
            break;
        }

        if (pwm == current_pwm) {
            continue;
        }

        if (pwm < current_pwm) {
            if (++slowdown_ticks < 8) {
                continue;
            }
            if (current_pwm - pwm > 5) {
                pwm = current_pwm - 5;
            }
        }

        DBG("%u ˚C -> %u", temp, pwm);

        if (!set_pwm(pwm)) {
            fprintf(stderr, "Failed to set pwm\n");
            break;
        }

        slowdown_ticks = 0;
        current_pwm = pwm;
    }

    set_pwm_manual(false);

    return closing ? 0 : 5;
}
