/*
 * image.c - OCI image handling
 *
 * Pull and resolve container images using libcurl.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include <errno.h>
#include <curl/curl.h>
#include "image.h"

/* Default cache directory */
#define DEFAULT_CACHE_DIR "/tmp/podvm/images"

/* Buffer sizes */
#define PATH_SIZE 2048

static char cache_dir[PATH_SIZE] = DEFAULT_CACHE_DIR;

/* Create directory recursively */
static int mkdir_p(const char *path, int mode) {
    char tmp[PATH_SIZE];
    char *p;
    
    snprintf(tmp, sizeof(tmp), "%s", path);
    for (p = tmp + 1; *p; p++) {
        if (*p == '/') {
            *p = '\0';
            if (mkdir(tmp, mode) < 0 && errno != EEXIST) return -1;
            *p = '/';
        }
    }
    if (mkdir(tmp, mode) < 0 && errno != EEXIST) return -1;
    return 0;
}

/* Write callback for curl */
static size_t write_callback(void *ptr, size_t size, size_t nmemb, void *userdata) {
    FILE *file = (FILE *)userdata;
    return fwrite(ptr, size, nmemb, file);
}

int image_parse(const char *image_str, ImageRef *ref) {
    if (!image_str || !ref) return -1;
    
    memset(ref, 0, sizeof(*ref));
    
    /* Find colon for tag */
    const char *colon = strrchr(image_str, ':');
    const char *slash = strrchr(image_str, '/');
    
    /* Check if colon is after last slash (i.e., it's a tag) */
    if (colon && (!slash || colon > slash)) {
        /* Has tag */
        size_t name_len = colon - image_str;
        if (name_len >= MAX_IMAGE_NAME) name_len = MAX_IMAGE_NAME - 1;
        memcpy(ref->name, image_str, name_len);
        strncpy(ref->tag, colon + 1, MAX_IMAGE_NAME - 1);
    } else {
        /* No tag, use latest */
        strncpy(ref->name, image_str, MAX_IMAGE_NAME - 1);
        strcpy(ref->tag, "latest");
    }
    
    return 0;
}

int image_is_path(const char *str) {
    if (!str) return 0;
    
    /* Absolute path */
    if (str[0] == '/') return 1;
    
    /* Relative path */
    if (strncmp(str, "./", 2) == 0 || strncmp(str, "../", 3) == 0) return 1;
    
    /* Check if it's an existing directory */
    struct stat st;
    if (stat(str, &st) == 0 && S_ISDIR(st.st_mode)) return 1;
    
    return 0;
}

const char *image_cache_dir(void) {
    return cache_dir;
}

int image_pull(const char *image_name, const char *tag) {
    printf("podvm: pulling image %s:%s...\n", image_name, tag);
    fflush(stdout);
    
    /* Initialize curl */
    curl_global_init(CURL_GLOBAL_DEFAULT);
    
    /* Create image directory */
    char image_dir[PATH_SIZE];
    snprintf(image_dir, sizeof(image_dir), "%s/%s/%s", cache_dir, image_name, tag);
    
    /* Create directories recursively */
    if (mkdir_p(cache_dir, 0755) < 0) {
        fprintf(stderr, "podvm: failed to create cache dir: %s\n", cache_dir);
        curl_global_cleanup();
        return -1;
    }
    
    char subdir[PATH_SIZE];
    snprintf(subdir, sizeof(subdir), "%s/%s", cache_dir, image_name);
    if (mkdir_p(subdir, 0755) < 0) {
        fprintf(stderr, "podvm: failed to create image dir: %s\n", subdir);
        curl_global_cleanup();
        return -1;
    }
    
    if (mkdir_p(image_dir, 0755) < 0) {
        fprintf(stderr, "podvm: failed to create tag dir: %s\n", image_dir);
        curl_global_cleanup();
        return -1;
    }
    
    /* TODO: Implement proper OCI registry pull
     * 
     * For now, create a placeholder image structure.
     * Real implementation would:
     * 1. Get auth token from registry
     * 2. Fetch manifest
     * 3. Download layers
     * 4. Extract layers to image_dir
     */
    
    /* Create minimal rootfs structure */
    char path[PATH_SIZE];
    
    snprintf(path, sizeof(path), "%s/bin", image_dir);
    mkdir(path, 0755);
    
    snprintf(path, sizeof(path), "%s/etc", image_dir);
    mkdir(path, 0755);
    
    snprintf(path, sizeof(path), "%s/usr", image_dir);
    mkdir(path, 0755);
    
    snprintf(path, sizeof(path), "%s/usr/bin", image_dir);
    mkdir(path, 0755);
    
    snprintf(path, sizeof(path), "%s/tmp", image_dir);
    mkdir(path, 0755);
    
    snprintf(path, sizeof(path), "%s/root", image_dir);
    mkdir(path, 0755);
    
    /* Create a minimal shell */
    snprintf(path, sizeof(path), "%s/bin/sh", image_dir);
    FILE *f = fopen(path, "w");
    if (f) {
        fprintf(f, "#!/bin/sh\n");
        fprintf(f, "echo \"Hello from podvm (image: %s:%s)\"\n", image_name, tag);
        fprintf(f, "if [ $# -gt 0 ]; then\n");
        fprintf(f, "    exec \"$@\"\n");
        fprintf(f, "fi\n");
        fprintf(f, "exit 0\n");
        fclose(f);
        chmod(path, 0755);
    }
    
    /* Create /bin/echo */
    snprintf(path, sizeof(path), "%s/bin/echo", image_dir);
    f = fopen(path, "w");
    if (f) {
        fprintf(f, "#!/bin/sh\n");
        fprintf(f, "echo \"$@\"\n");
        fclose(f);
        chmod(path, 0755);
    }
    
    /* Create /etc/os-release */
    snprintf(path, sizeof(path), "%s/etc/os-release", image_dir);
    f = fopen(path, "w");
    if (f) {
        fprintf(f, "NAME=\"%s\"\n", image_name);
        fprintf(f, "VERSION=\"%s\"\n", tag);
        fprintf(f, "ID=%s\n", image_name);
        fprintf(f, "PRETTY_NAME=\"%s %s\"\n", image_name, tag);
        fclose(f);
    }
    
    printf("podvm: image %s:%s ready at %s\n", image_name, tag, image_dir);
    printf("podvm: (note: using placeholder image - full registry pull not implemented)\n");
    fflush(stdout);
    
    curl_global_cleanup();
    return 0;
}

int image_resolve(const char *image_str, char *rootfs_path, size_t path_size) {
    if (!image_str || !rootfs_path) return -1;
    
    /* If it's a path, use directly */
    if (image_is_path(image_str)) {
        strncpy(rootfs_path, image_str, path_size - 1);
        rootfs_path[path_size - 1] = '\0';
        
        /* Validate path exists */
        struct stat st;
        if (stat(rootfs_path, &st) < 0 || !S_ISDIR(st.st_mode)) {
            fprintf(stderr, "podvm: directory not found: %s\n", rootfs_path);
            return -1;
        }
        return 0;
    }
    
    /* Parse image name */
    ImageRef ref;
    if (image_parse(image_str, &ref) < 0) {
        fprintf(stderr, "podvm: failed to parse image name: %s\n", image_str);
        return -1;
    }
    
    /* Check cache */
    char image_dir[PATH_SIZE];
    snprintf(image_dir, sizeof(image_dir), "%s/%s/%s", cache_dir, ref.name, ref.tag);
    
    struct stat st;
    if (stat(image_dir, &st) == 0 && S_ISDIR(st.st_mode)) {
        /* Image already cached */
        strncpy(rootfs_path, image_dir, path_size - 1);
        rootfs_path[path_size - 1] = '\0';
        printf("podvm: using cached image %s:%s\n", ref.name, ref.tag);
        return 0;
    }
    
    /* Try to pull */
    if (image_pull(ref.name, ref.tag) < 0) {
        fprintf(stderr, "podvm: failed to pull image: %s:%s\n", ref.name, ref.tag);
        return -1;
    }
    
    /* Validate pulled image */
    if (stat(image_dir, &st) < 0 || !S_ISDIR(st.st_mode)) {
        fprintf(stderr, "podvm: image directory not found after pull: %s\n", image_dir);
        return -1;
    }
    
    /* Use pulled image */
    strncpy(rootfs_path, image_dir, path_size - 1);
    rootfs_path[path_size - 1] = '\0';
    
    return 0;
}
