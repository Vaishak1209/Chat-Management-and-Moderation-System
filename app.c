#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/wait.h>
#include <sys/msg.h>
#include <sys/types.h>
#include <errno.h>
#include <string.h>

#define MAX_USERS 50
#define MAX_GROUPS 30

struct message {
    long mtype;
    int group_id;
};
struct AppMessage {
    long mtype;
    int group_id;
};
int main(int argc, char *argv[]) {
    if (argc != 2) {
        fprintf(stderr, "Usage: %s <test_case_number>\n", argv[0]);
        exit(1);
    }

    int test_case = atoi(argv[1]);
    char testcase_folder[256];
    snprintf(testcase_folder, sizeof(testcase_folder), "testcase_%d", test_case);
    printf("Using testcase folder: %s\n", testcase_folder);

    int n, validation_queue_key, app_groups_queue_key, moderator_groups_queue_key, threshold;
    char group_paths[MAX_GROUPS][256];

    char input_file_path[256];
    snprintf(input_file_path, sizeof(input_file_path), "%s/input.txt", testcase_folder);
    FILE *input_file = fopen(input_file_path, "r");
    if (input_file == NULL) {
        fprintf(stderr, "Error opening %s: %s\n", input_file_path, strerror(errno));
        exit(1);
    }

    if (fscanf(input_file, "%d %d %d %d %d", &n, &validation_queue_key,
               &app_groups_queue_key, &moderator_groups_queue_key, &threshold) != 5) {
        fprintf(stderr, "Error reading from input.txt: Invalid format\n");
        exit(1);
    }

    printf("Read from input.txt: n=%d, validation_key=%d, app_key=%d, moderator_key=%d, threshold=%d\n",
           n, validation_queue_key, app_groups_queue_key, moderator_groups_queue_key, threshold);

    for (int i = 0; i < n; i++) {
        if (fscanf(input_file, "%255s", group_paths[i]) != 1) {
            fprintf(stderr, "Error reading group file paths from input.txt\n");
            exit(1);
        }
        printf("Group %d path: %s\n", i, group_paths[i]);
    }
    fclose(input_file);

    if (n <= 0 || n > MAX_GROUPS) {
        fprintf(stderr, "Invalid number of groups: %d\n", n);
        exit(1);
    }

    int app_msgid = msgget(app_groups_queue_key, IPC_CREAT | 0666);
    if (app_msgid == -1) {
        fprintf(stderr, "Error creating app groups message queue (key: %d): %s\n",
                app_groups_queue_key, strerror(errno));
        exit(1);
    }
    printf("Successfully created app groups message queue (id: %d)\n", app_msgid);

    int moderator_msgid = msgget(moderator_groups_queue_key, IPC_CREAT | 0666);
    if (moderator_msgid == -1) {
        fprintf(stderr, "Error creating moderator groups message queue (key: %d): %s\n",
                moderator_groups_queue_key, strerror(errno));
        exit(1);
    }
    printf("Successfully created moderator message queue (id: %d)\n", moderator_msgid);

    pid_t pids[MAX_GROUPS];
    int active_groups = n;

    for (int i = 0; i < n; i++) {
        pids[i] = fork();
        if (pids[i] == -1) {
            fprintf(stderr, "Fork failed for group %d: %s\n", i, strerror(errno));
            exit(1);
        } else if (pids[i] == 0) {
            // Child process
            char group_id_str[10];
            snprintf(group_id_str, sizeof(group_id_str), "%d", i);

            char test_case_str[10];
            snprintf(test_case_str, sizeof(test_case_str), "%d", test_case);

            execl("./groups.out", "groups.out", group_id_str, test_case_str, (char *)NULL);

            fprintf(stderr, "execl failed for group %d: %s\n", i, strerror(errno));
            exit(1);
        }
    }

    //struct message msg;
    struct AppMessage msg;
    while (active_groups > 0) {
        printf("Waiting for message from groups...\n");
        if (msgrcv(app_msgid, &msg, sizeof(msg.group_id), 0, 0) == -1) {
            if (errno == EINTR) {
                printf("Interrupted while waiting for message. Retrying...\n");
                continue;
            }
            fprintf(stderr, "Error receiving message: %s\n", strerror(errno));
            exit(1);
        }

        printf("Received termination from group %d\n", msg.group_id);
    active_groups--;
    }

    printf("All groups have terminated. Waiting for child processes to finish...\n");
    for (int i = 0; i < n; i++) {
        waitpid(pids[i], NULL, 0);
        printf("Child process for group %d has finished\n", i);
    }

    printf("Removing message queues...\n");
    if (msgctl(app_msgid, IPC_RMID, NULL) == -1) {
        fprintf(stderr, "Error removing app message queue: %s\n", strerror(errno));
        exit(1);
    }
    if (msgctl(moderator_msgid, IPC_RMID, NULL) == -1) {
        fprintf(stderr, "Error removing moderator message queue: %s\n", strerror(errno));
        exit(1);
    }

    printf("App process completed successfully\n");
    return 0;
}
