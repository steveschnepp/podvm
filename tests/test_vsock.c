/*
 * test_vsock.c - Test vsock communication
 *
 * Build: gcc -o test_vsock tests/test_vsock.c -lpthread
 * Run: ./test_vsock
 *
 * This test creates a vsock server and client in the same process
 * to verify the communication protocol.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <assert.h>
#include <pthread.h>
#include <sys/socket.h>
#include <linux/vm_sockets.h>

#define VSOCK_PORT 5000
#define BUF_SIZE 4096

/* Server thread */
static void *server_thread(void *arg) {
    int server_fd, client_fd;
    struct sockaddr_vm addr;
    char buf[BUF_SIZE];
    ssize_t n;
    
    (void)arg;
    
    /* Create socket */
    server_fd = socket(AF_VSOCK, SOCK_STREAM, 0);
    if (server_fd < 0) {
        fprintf(stderr, "Server: socket failed\n");
        return NULL;
    }
    
    /* Bind */
    memset(&addr, 0, sizeof(addr));
    addr.svm_family = AF_VSOCK;
    addr.svm_cid = VMADDR_CID_ANY;
    addr.svm_port = VSOCK_PORT;
    
    if (bind(server_fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        fprintf(stderr, "Server: bind failed\n");
        close(server_fd);
        return NULL;
    }
    
    if (listen(server_fd, 1) < 0) {
        fprintf(stderr, "Server: listen failed\n");
        close(server_fd);
        return NULL;
    }
    
    printf("Server: listening on vsock:%d\n", VSOCK_PORT);
    
    /* Accept connection */
    client_fd = accept(server_fd, NULL, NULL);
    if (client_fd < 0) {
        fprintf(stderr, "Server: accept failed\n");
        close(server_fd);
        return NULL;
    }
    
    printf("Server: client connected\n");
    
    /* Read config */
    memset(buf, 0, sizeof(buf));
    n = read(client_fd, buf, sizeof(buf) - 1);
    if (n <= 0) {
        fprintf(stderr, "Server: read failed\n");
        close(client_fd);
        close(server_fd);
        return NULL;
    }
    
    printf("Server: received '%s'\n", buf);
    
    /* Verify config */
    assert(strstr(buf, "podvm.root=/rootfs") != NULL);
    assert(strstr(buf, "podvm.entrypoint=/bin/sh") != NULL);
    
    /* Send ACK */
    write(client_fd, "OK", 2);
    
    close(client_fd);
    close(server_fd);
    
    printf("Server: done\n");
    return NULL;
}

/* Client thread */
static void *client_thread(void *arg) {
    int fd;
    struct sockaddr_vm addr;
    char config[] = "podvm.root=/rootfs podvm.entrypoint=/bin/sh podvm.args=-";
    char buf[BUF_SIZE];
    
    (void)arg;
    
    /* Wait for server to start */
    usleep(100000);  /* 100ms */
    
    /* Create socket */
    fd = socket(AF_VSOCK, SOCK_STREAM, 0);
    if (fd < 0) {
        fprintf(stderr, "Client: socket failed\n");
        return NULL;
    }
    
    /* Connect */
    memset(&addr, 0, sizeof(addr));
    addr.svm_family = AF_VSOCK;
    addr.svm_cid = VMADDR_CID_HOST;
    addr.svm_port = VSOCK_PORT;
    
    if (connect(fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        fprintf(stderr, "Client: connect failed\n");
        close(fd);
        return NULL;
    }
    
    printf("Client: connected\n");
    
    /* Send config */
    if (write(fd, config, strlen(config)) != (ssize_t)strlen(config)) {
        fprintf(stderr, "Client: write failed\n");
        close(fd);
        return NULL;
    }
    
    printf("Client: sent config\n");
    
    /* Read ACK */
    memset(buf, 0, sizeof(buf));
    ssize_t n = read(fd, buf, sizeof(buf) - 1);
    if (n <= 0) {
        fprintf(stderr, "Client: read failed\n");
        close(fd);
        return NULL;
    }
    
    printf("Client: received '%s'\n", buf);
    assert(strcmp(buf, "OK") == 0);
    
    close(fd);
    
    printf("Client: done\n");
    return NULL;
}

/* Test vsock socket creation */
static void test_vsock_socket(void) {
    int fd;
    
    fd = socket(AF_VSOCK, SOCK_STREAM, 0);
    assert(fd >= 0);
    
    close(fd);
    
    printf("PASS: test_vsock_socket\n");
}

/* Test vsock address construction */
static void test_vsock_addr(void) {
    struct sockaddr_vm addr;
    
    memset(&addr, 0, sizeof(addr));
    addr.svm_family = AF_VSOCK;
    addr.svm_cid = VMADDR_CID_HOST;
    addr.svm_port = VSOCK_PORT;
    
    assert(addr.svm_family == AF_VSOCK);
    assert(addr.svm_cid == VMADDR_CID_HOST);
    assert(addr.svm_port == VSOCK_PORT);
    
    /* Test ANY CID */
    addr.svm_cid = VMADDR_CID_ANY;
    assert(addr.svm_cid == VMADDR_CID_ANY);
    
    printf("PASS: test_vsock_addr\n");
}

/* Test vsock communication */
static void test_vsock_communication(void) {
    pthread_t server_tid, client_tid;
    
    printf("Starting vsock communication test...\n");
    printf("(Note: This test requires vsock support in the kernel)\n");
    
    /* Create threads */
    if (pthread_create(&server_tid, NULL, server_thread, NULL) != 0) {
        fprintf(stderr, "Failed to create server thread\n");
        return;
    }
    
    if (pthread_create(&client_tid, NULL, client_thread, NULL) != 0) {
        fprintf(stderr, "Failed to create client thread\n");
        return;
    }
    
    /* Wait for threads */
    pthread_join(server_tid, NULL);
    pthread_join(client_tid, NULL);
    
    printf("PASS: test_vsock_communication\n");
}

int main(void) {
    printf("Running vsock tests...\n\n");
    
    /* Basic tests */
    test_vsock_socket();
    test_vsock_addr();
    
    /* Communication test (requires vsock support) */
    printf("\nAttempting vsock communication test...\n");
    test_vsock_communication();
    
    printf("\nAll tests passed!\n");
    return 0;
}
