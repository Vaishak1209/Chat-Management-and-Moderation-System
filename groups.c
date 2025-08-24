#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/msg.h>
#include <string.h>
#include <fcntl.h>
#include <errno.h>
#include <signal.h>

#define MAX_USERS 50
#define MAX_MESSAGE_LENGTH 256
#define MAX_NUMBER_OF_GROUPS 30
#define MAX_GROUPS 30
#define MAX_TIMESTAMP 2147000000

typedef struct {
    long mtype;
    int timestamp;
    int user;
    char mtext[256];
    int modifyingGroup;
} Message;

struct User {
    int id;
    int pipe_fd[2];
    pid_t pid;
};
struct AppMessage {
    long mtype;
    int group_id;
};
int group_id, validation_queue_key, app_groups_queue_key, moderator_groups_queue_key;
int validation_queue_id, app_groups_queue_id, moderator_groups_queue_id;
struct User users[MAX_USERS];
int user_count = 0;
int removed_users = 0;

void send_validation_message(int mtype, int user) {
    Message msg = {.mtype = mtype, .modifyingGroup = group_id, .user = user};
    if (msgsnd(validation_queue_id, &msg, sizeof(Message) - sizeof(long), 0) == -1) {
        fprintf(stderr, "Error sending validation message: %s\n", strerror(errno));
        exit(1);
    }
    printf("Sent validation message: type=%d, group=%d, user=%d\n", mtype, group_id, user);
}

void add_user(const char* user_file) {
    if (user_count >= MAX_USERS) {
   
        fprintf(stderr, "Error: Cannot add user to group %d as group is already full,Current users:%d.\n", group_id,user_count);
        return;
    }

    if (pipe(users[user_count].pipe_fd) == -1) {
        fprintf(stderr, "Error creating pipe: %s\n", strerror(errno));
        exit(1);
    }

    users[user_count].pid = fork();
    if (users[user_count].pid == -1) {
        fprintf(stderr, "Error forking: %s\n", strerror(errno));
        exit(1);
    } else if (users[user_count].pid == 0) {
        // Child process (user)
        close(users[user_count].pipe_fd[0]);
        FILE* file = fopen(user_file, "r");
        if (!file) {
            fprintf(stderr, "Error opening user file %s: %s\n", user_file, strerror(errno));
            exit(1);
        }

        char line[MAX_MESSAGE_LENGTH];
        while (fgets(line, sizeof(line), file)) {
            ssize_t len = strlen(line);
            if (write(users[user_count].pipe_fd[1], line, len) != len) {
                fprintf(stderr, "Write error to pipe\n");
                exit(1);
            }
        }

        fclose(file);
        close(users[user_count].pipe_fd[1]);
        exit(0);
    } else {
        // Parent process (group)
        close(users[user_count].pipe_fd[1]);
        users[user_count].id = user_count;
        send_validation_message(2, user_count);
        user_count++;
        printf("Added user %d from file %s\n", user_count - 1, user_file);
    }
}

void remove_user(int user_index) {
    if (user_index < 0 || user_index >= user_count) {
        fprintf(stderr, "Invalid user index to remove.\n");
        return;
    }
    close(users[user_index].pipe_fd[0]);
    kill(users[user_index].pid, SIGTERM);
    waitpid(users[user_index].pid, NULL, 0);

    for (int i = user_index; i < user_count - 1; i++) {
        users[i] = users[i + 1];
        users[i].id = i;
    }
    user_count--;
    printf("Removed user %d, remaining users: %d\n", user_index, user_count);
}

void process_user_messages() {
    fd_set read_fds;
    int max_fd = -1;
    Message val_msg, mod_response;
    struct timeval tv;
    tv.tv_sec = 0;
    tv.tv_usec = 10000;

    while (user_count > 0) {
        FD_ZERO(&read_fds);
        max_fd = -1;
        for (int i = 0; i < user_count; i++) {
            FD_SET(users[i].pipe_fd[0], &read_fds);
            if (users[i].pipe_fd[0] > max_fd) {
                max_fd = users[i].pipe_fd[0];
            }
        }

        if (max_fd == -1) break;

        int select_result = select(max_fd + 1, &read_fds, NULL, NULL, &tv);
        if (select_result == -1) {
            if (errno == EINTR) continue;
            fprintf(stderr, "Error in select: %s\n", strerror(errno));
            exit(1);
        } else if (select_result == 0) {
            continue;
        }

        for (int i = 0; i < user_count; i++) {
            if (FD_ISSET(users[i].pipe_fd[0], &read_fds)) {
                char buffer[MAX_MESSAGE_LENGTH];
                ssize_t bytes_read = read(users[i].pipe_fd[0], buffer, sizeof(buffer) - 1);
                if (bytes_read > 0) {
                    buffer[bytes_read] = '\0';
                    int timestamp;
                    char text[MAX_MESSAGE_LENGTH];
                    if (sscanf(buffer, "%d %[^\n]", &timestamp, text) != 2) {
                        fprintf(stderr, "Error parsing message from user %d\n", users[i].id);
                        continue;
                    }

                    if (timestamp > MAX_TIMESTAMP) {
                        fprintf(stderr, "Error: Timestamp exceeds maximum allowed value\n");
                        continue;
                    }

                    val_msg.mtype = MAX_NUMBER_OF_GROUPS + group_id;
                    val_msg.timestamp = timestamp;
                    val_msg.user = users[i].id;
                    val_msg.modifyingGroup = group_id;
                    strncpy(val_msg.mtext, text, sizeof(val_msg.mtext) - 1);
                    val_msg.mtext[sizeof(val_msg.mtext) - 1] = '\0';
                   
                    if (msgsnd(validation_queue_id, &val_msg, sizeof(Message) - sizeof(long), 0) == -1) {
                   
                        fprintf(stderr, "Error sending message to validation: %s\n", strerror(errno));
                        exit(1);
                    }
                    printf("Sent message to validation: user=%d, timestamp=%d, text=%s\n", val_msg.user, val_msg.timestamp, val_msg.mtext);

                    if (msgsnd(moderator_groups_queue_id, &val_msg, sizeof(Message) - sizeof(long), 0) == -1) {
                        fprintf(stderr, "Error sending message to moderator: %s\n", strerror(errno));
                        exit(1);
                    }
                    printf("Sent message to moderator: user=%d, timestamp=%d, text=%s\n", val_msg.user, val_msg.timestamp, val_msg.mtext);

                    if (msgrcv(moderator_groups_queue_id, &mod_response, sizeof(Message) - sizeof(long), group_id, IPC_NOWAIT) != -1) {
                        if (mod_response.user == users[i].id) {
                            printf("Received removal request from moderator for user %d\n", mod_response.user);
                            remove_user(i);
                            removed_users++;
                            i--;
                        }
                    }
                } else if (bytes_read == 0) {
                    printf("User %d has sent all messages\n", users[i].id);
                    remove_user(i);
                    removed_users++;
                    i--;
                } else {
                    perror("Read error");
                }
            }
        }
    }
}

int main(int argc, char *argv[]) {
    if (argc != 3) {
        fprintf(stderr, "Usage: %s <group_id> <test_case_number>\n", argv[0]);
        exit(1);
    }

    group_id = atoi(argv[1]);
    int test_case = atoi(argv[2]);
    printf("Starting group %d for test case %d\n", group_id, test_case);

    char testcase_folder[256];
    snprintf(testcase_folder, sizeof(testcase_folder), "testcase_%d", test_case);

    char input_file_path[512];
    snprintf(input_file_path, sizeof(input_file_path), "%s/input.txt", testcase_folder);
   
    int n, threshold;
    char group_paths[MAX_GROUPS][256];

    FILE* input_file = fopen(input_file_path, "r");
    if (!input_file) {
        fprintf(stderr, "Error opening input file %s: %s\n", input_file_path, strerror(errno));
        exit(1);
    }

    if (fscanf(input_file, "%d %d %d %d %d", &n, &validation_queue_key,
               &app_groups_queue_key, &moderator_groups_queue_key, &threshold) != 5) {
        fprintf(stderr, "Error reading from input file: Invalid format\n");
        exit(1);
    }
    printf("Read from input file: n=%d, validation_key=%d, app_key=%d, moderator_key=%d, threshold=%d\n",
           n, validation_queue_key, app_groups_queue_key, moderator_groups_queue_key, threshold);

    for (int i = 0; i < n; i++) {
        if (fscanf(input_file, "%s", group_paths[i]) != 1) {
            fprintf(stderr, "Error reading group file paths from input file\n");
            exit(1);
        }
        printf("Group %d path: %s\n", i, group_paths[i]);
    }
    fclose(input_file);

    validation_queue_id = msgget(validation_queue_key, 0666);
    app_groups_queue_id = msgget(app_groups_queue_key, 0666);
    moderator_groups_queue_id = msgget(moderator_groups_queue_key, 0666);

    if (validation_queue_id == -1 || app_groups_queue_id == -1 || moderator_groups_queue_id == -1) {
        fprintf(stderr, "Error connecting to message queues: %s\n", strerror(errno));
        exit(1);
    }
    printf("Connected to all message queues successfully\n");

    send_validation_message(1, 0);

    char group_file_path[512];
    snprintf(group_file_path, sizeof(group_file_path), "%s/%s", testcase_folder, group_paths[group_id]);
    FILE* group_fp = fopen(group_file_path, "r");
    if (!group_fp) {
        fprintf(stderr, "Error opening group file %s: %s\n", group_file_path, strerror(errno));
        exit(1);
    }

    int M;
    if (fscanf(group_fp, "%d", &M) != 1) {
        fprintf(stderr, "Error reading number of users from group file\n");
        exit(1);
    }
    if (M > MAX_USERS) {
        fprintf(stderr, "Error: Group %d specifies %d users, which exceeds the maximum of %d\n", group_id, M, MAX_USERS);
        exit(1);
    }
    printf("Number of users in group %d: %d\n", group_id, M);

    char user_file[256];
    for (int i = 0; i < M; i++) {
        if (fscanf(group_fp, "%s", user_file) != 1) {
            fprintf(stderr, "Error reading user file path from group file\n");
            exit(1);
        }
        char full_user_file_path[512];
        snprintf(full_user_file_path, sizeof(full_user_file_path), "%s/%s", testcase_folder, user_file);
        add_user(full_user_file_path);
    }

    fclose(group_fp);

    process_user_messages();
    struct AppMessage terminate_msg;
    terminate_msg.mtype = group_id;
    terminate_msg.group_id = group_id;

    if (msgsnd(app_groups_queue_id, &terminate_msg, sizeof(struct AppMessage) - sizeof(long), 0) == -1) {
    fprintf(stderr, "Error sending termination message: %s\n", strerror(errno));
}
    send_validation_message(3, removed_users);
    printf("Group %d terminated. Total removed users: %d\n", group_id, removed_users);

    return 0;
}
