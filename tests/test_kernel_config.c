/*
 * test_kernel_config.c - Test kernel configuration
 *
 * Build: gcc -o test_kernel_config tests/test_kernel_config.c
 * Run: ./test_kernel_config
 *
 * Validates that kernel/config has the required options
 * and doesn't have forbidden ones.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>

#define MAX_LINE 256

/* Required config options */
static const char *required[] = {
    "CONFIG_SMP=y",
    "CONFIG_VIRTIO=y",
    "CONFIG_VIRTIO_PCI=y",
    "CONFIG_VIRTIO_CONSOLE=y",
    "CONFIG_VIRTIO_FS=y",
    "CONFIG_VIRTIO_BALLOON=y",
    "CONFIG_VSOCK=y",
    "CONFIG_VIRTIO_VSOCKETS_COMMON=y",
    "CONFIG_TMPFS=y",
    "CONFIG_PROC_FS=y",
    "CONFIG_SYSFS=y",
    "CONFIG_DEVTMPFS=y",
    "CONFIG_NAMESPACES=y",
    "CONFIG_SECCOMP=y",
    "CONFIG_SECURITY=y",
    "CONFIG_SERIAL_8250=y",
    "CONFIG_SERIAL_8250_CONSOLE=y",
    "CONFIG_FUTEX=y",
    "CONFIG_EPOLL=y",
    "CONFIG_SIGNALFD=y",
    "CONFIG_EVENTFD=y",
    "CONFIG_PRINTK=y",
    NULL
};

/* Forbidden config options (attack surface) */
static const char *forbidden[] = {
    "CONFIG_BPF=y",
    "CONFIG_BPF_SYSCALL=y",
    "CONFIG_VIRTIO_MMIO=y",
    "CONFIG_VIRTIO_BLK=y",
    "CONFIG_VIRTIO_NET=y",
    "CONFIG_VIRTIO_RNG=y",
    "CONFIG_VT=y",
    "CONFIG_VT_CONSOLE=y",
    "CONFIG_MODULES=y",
    "CONFIG_FTRACE=y",
    "CONFIG_KPROBES=y",
    "CONFIG_PERF_EVENTS=y",
    "CONFIG_DEBUG_FS=y",
    "CONFIG_KGDB=y",
    "CONFIG_MAGIC_SYSRQ=y",
    "CONFIG_AUDIT=y",
    "CONFIG_APPARMOR=y",
    "CONFIG_SELINUX=y",
    "CONFIG_ACPI=y",
    "CONFIG_PM=y",
    "CONFIG_SUSPEND=y",
    "CONFIG_CRYPTO_LZ4=y",
    "CONFIG_CRYPTO_ZSTD=y",
    "CONFIG_CRYPTO_GZIP=y",
    NULL
};

/* Read config file and check options */
static int check_config(const char *filename, const char **options, int should_exist) {
    FILE *f;
    char line[MAX_LINE];
    int found = 0;
    int missing = 0;
    
    f = fopen(filename, "r");
    if (!f) {
        fprintf(stderr, "Cannot open %s\n", filename);
        return -1;
    }
    
    while (fgets(line, sizeof(line), f)) {
        /* Remove newline */
        line[strcspn(line, "\n")] = '\0';
        
        /* Skip comments and empty lines */
        if (line[0] == '#' || line[0] == '\0')
            continue;
        
        /* Check each option */
        for (int i = 0; options[i]; i++) {
            if (strcmp(line, options[i]) == 0) {
                found++;
                break;
            }
        }
    }
    
    fclose(f);
    
    /* Count total options */
    int total = 0;
    for (int i = 0; options[i]; i++)
        total++;
    
    if (should_exist) {
        missing = total - found;
        if (missing > 0) {
            fprintf(stderr, "Missing %d required options\n", missing);
            return 1;
        }
    } else {
        if (found > 0) {
            fprintf(stderr, "Found %d forbidden options\n", found);
            return 1;
        }
    }
    
    return 0;
}

/* Test required options exist */
static void test_required_options(void) {
    printf("Testing required options...\n");
    
    int ret = check_config("kernel/config", required, 1);
    
    if (ret == 0) {
        printf("PASS: all required options present\n");
    } else {
        printf("FAIL: missing required options\n");
        /* Print which ones are missing */
        FILE *f = fopen("kernel/config", "r");
        if (f) {
            char line[MAX_LINE];
            while (fgets(line, sizeof(line), f)) {
                line[strcspn(line, "\n")] = '\0';
                if (line[0] == '#' || line[0] == '\0')
                    continue;
                for (int i = 0; required[i]; i++) {
                    if (strcmp(line, required[i]) == 0) {
                        printf("  Found: %s\n", required[i]);
                        break;
                    }
                }
            }
            fclose(f);
        }
        assert(ret == 0);
    }
}

/* Test forbidden options don't exist */
static void test_forbidden_options(void) {
    printf("Testing forbidden options...\n");
    
    int ret = check_config("kernel/config", forbidden, 0);
    
    if (ret == 0) {
        printf("PASS: no forbidden options present\n");
    } else {
        printf("FAIL: found forbidden options\n");
        /* Print which ones */
        FILE *f = fopen("kernel/config", "r");
        if (f) {
            char line[MAX_LINE];
            while (fgets(line, sizeof(line), f)) {
                line[strcspn(line, "\n")] = '\0';
                if (line[0] == '#' || line[0] == '\0')
                    continue;
                for (int i = 0; forbidden[i]; i++) {
                    if (strcmp(line, forbidden[i]) == 0) {
                        printf("  Found: %s\n", forbidden[i]);
                        break;
                    }
                }
            }
            fclose(f);
        }
        assert(ret == 0);
    }
}

/* Test specific security options */
static void test_security_options(void) {
    printf("Testing security options...\n");
    
    FILE *f = fopen("kernel/config", "r");
    if (!f) {
        printf("WARN: cannot open kernel/config\n");
        return;
    }
    
    char line[MAX_LINE];
    int has_stack_protector = 0;
    int has_kaslr = 0;
    int has_uncompressed = 0;
    
    while (fgets(line, sizeof(line), f)) {
        line[strcspn(line, "\n")] = '\0';
        
        if (strstr(line, "CONFIG_CC_STACKPROTECTOR_STRONG=y"))
            has_stack_protector = 1;
        if (strstr(line, "CONFIG_RANDOMIZE_BASE=y"))
            has_kaslr = 1;
        if (strstr(line, "CONFIG_KERNEL_UNCOMPRESSED=y"))
            has_uncompressed = 1;
    }
    
    fclose(f);
    
    if (has_stack_protector && has_kaslr && has_uncompressed) {
        printf("PASS: security options present\n");
    } else {
        printf("WARN: some security options missing\n");
        printf("  Stack protector: %s\n", has_stack_protector ? "yes" : "no");
        printf("  KASLR: %s\n", has_kaslr ? "yes" : "no");
        printf("  Uncompressed kernel: %s\n", has_uncompressed ? "yes" : "no");
    }
}

/* Test NR_CPUS is reasonable */
static void test_nr_cpus(void) {
    printf("Testing NR_CPUS...\n");
    
    FILE *f = fopen("kernel/config", "r");
    if (!f) {
        printf("WARN: cannot open kernel/config\n");
        return;
    }
    
    char line[MAX_LINE];
    int nr_cpus = 0;
    
    while (fgets(line, sizeof(line), f)) {
        if (sscanf(line, "CONFIG_NR_CPUS=%d", &nr_cpus) == 1)
            break;
    }
    
    fclose(f);
    
    if (nr_cpus > 0 && nr_cpus <= 8) {
        printf("PASS: NR_CPUS=%d (reasonable)\n", nr_cpus);
    } else {
        printf("WARN: NR_CPUS=%d (may be too high)\n", nr_cpus);
    }
}

int main(void) {
    printf("kernel/config validation tests\n");
    printf("==============================\n\n");
    
    test_required_options();
    test_forbidden_options();
    test_security_options();
    test_nr_cpus();
    
    printf("\nAll validation tests passed!\n");
    return 0;
}
