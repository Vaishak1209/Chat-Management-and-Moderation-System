#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <sys/msg.h>
#include <sys/ipc.h>
#include <unistd.h>
#include <errno.h>

#define MAX_FILTERED_WORDS 50
#define MAX_WORD_LENGTH 20
#define MAX_MESSAGE_LENGTH 256
#define MAX_USERS 50
#define MAX_GROUPS 30
#define MAX_TIMESTAMP 2147000000

typedef struct {
    long mtype;
    int timestamp;
    int user;
    char mtext[256];
    int modifyingGroup;
} Message;

struct FilteredWord {
    char word[MAX_WORD_LENGTH];
};

struct UserViolations {
    int group_id;
    int user_id;
    int violations;
};

struct FilteredWord filtered_words[MAX_FILTERED_WORDS];
int filtered_word_count = 0;
struct UserViolations user_violations[MAX_USERS * MAX_GROUPS];
int user_violations_count = 0;
int threshold_violations;

void load_filtered_words(const char* testcase_folder) {
    char filename[256];
    snprintf(filename, sizeof(filename), "%s/filtered_words.txt", testcase_folder);
    FILE* file = fopen(filename, "r");
    if (file == NULL) {
        fprintf(stderr, "Error opening %s: %s\n", filename, strerror(errno));
        exit(1);
    }

    while (filtered_word_count < MAX_FILTERED_WORDS &&
           fscanf(file, "%19s", filtered_words[filtered_word_count].word) == 1) {
        filtered_word_count++;
    }

    fclose(file);
}

void to_lower(char* str) {
    for (int i = 0; str[i]; i++) {
        str[i] = tolower((unsigned char)str[i]);
    }
}

int count_violations(const char* message) {
    char lower_message[MAX_MESSAGE_LENGTH];
    strncpy(lower_message, message, MAX_MESSAGE_LENGTH - 1);
    lower_message[MAX_MESSAGE_LENGTH - 1] = '\0';
    to_lower(lower_message);

    int violations = 0;
    for (int i = 0; i < filtered_word_count; i++) {
        char lower_word[MAX_WORD_LENGTH];
        strncpy(lower_word, filtered_words[i].word, MAX_WORD_LENGTH - 1);
        lower_word[MAX_WORD_LENGTH - 1] = '\0';
        to_lower(lower_word);

        if (strstr(lower_message, lower_word) != NULL) {
            violations++;
        }
    }

    return violations;
}

void update_violations(int group_id, int user_id, int new_violations) {
    // Prevent array overflow
    if (user_violations_count >= MAX_USERS * MAX_GROUPS) {
        fprintf(stderr, "User violations storage full!\n");
        return;
    }
    for (int i = 0; i < user_violations_count; i++) {
        if (user_violations[i].group_id == group_id && user_violations[i].user_id == user_id) {
            user_violations[i].violations += new_violations;
            return;
        }
    }

    if (user_violations_count < MAX_USERS * MAX_GROUPS) {
        user_violations[user_violations_count].group_id = group_id;
        user_violations[user_violations_count].user_id = user_id;
        user_violations[user_violations_count].violations = new_violations;
        user_violations_count++;
    }
}

int main(int argc, char *argv[]) {
    if (argc != 2) {
        fprintf(stderr, "Usage: %s <test_case_number>\n", argv[0]);
        exit(1);
    }

    int test_case = atoi(argv[1]);
    char testcase_folder[256];
    snprintf(testcase_folder, sizeof(testcase_folder), "testcase_%d", test_case);
    printf("Using testcase folder: %s\n", testcase_folder);

    int n, validation_queue_key, app_groups_queue_key, moderator_groups_queue_key;
    int moderator_msgid;

    load_filtered_words(testcase_folder);
   
    char input_file_path[256];
    snprintf(input_file_path, sizeof(input_file_path), "%s/input.txt", testcase_folder);
    FILE* input_file = fopen(input_file_path, "r");
    if (input_file == NULL) {
        fprintf(stderr, "Error opening %s: %s\n", input_file_path, strerror(errno));
        exit(1);
    }

    if (fscanf(input_file, "%d %d %d %d %d",
    &n, &validation_queue_key, &app_groups_queue_key,
    &moderator_groups_queue_key, &threshold_violations) != 5) {
        fprintf(stderr, "Error reading from input.txt: Invalid format\n");
        exit(1);
    }

    printf("Read from input.txt: n=%d, validation_key=%d, app_key=%d, moderator_key=%d, threshold=%d\n",
    n, validation_queue_key, app_groups_queue_key, moderator_groups_queue_key, threshold_violations);

    fclose(input_file);

    moderator_msgid = msgget(moderator_groups_queue_key, 0666);
    if (moderator_msgid == -1) {
        fprintf(stderr, "Error in msgget for moderator (key: %d): %s\n",
        moderator_groups_queue_key, strerror(errno));
        exit(1);
    }
    printf("Successfully connected to moderator message queue (id: %d)\n", moderator_msgid);

    Message msg;
    while (1) {
        printf("Waiting for message...\n");
        if (msgrcv(moderator_msgid, &msg, sizeof(Message) - sizeof(long), 0, 0) == -1) {
            if (errno == EINTR) continue;
            fprintf(stderr, "Error in msgrcv: %s\n", strerror(errno));
            exit(1);
        }

        if (msg.timestamp > MAX_TIMESTAMP) {
            fprintf(stderr, "Error: Timestamp exceeds maximum allowed value\n");
            continue;
        }

        printf("Received message from group %d, user %d: %s\n",
        msg.modifyingGroup, msg.user, msg.mtext);

        int violations = count_violations(msg.mtext);
        update_violations(msg.modifyingGroup, msg.user, violations);

        int total_violations = 0;
        for (int i = 0; i < user_violations_count; i++) {
            if (user_violations[i].group_id == msg.modifyingGroup && user_violations[i].user_id == msg.user) {
                total_violations = user_violations[i].violations;
                break;
            }
        }

        printf("User %d from group %d has %d violations\n",
        msg.user, msg.modifyingGroup, total_violations);

        if (total_violations >= threshold_violations) {
            printf("User %d from group %d has been removed due to %d violations.\n",
            msg.user, msg.modifyingGroup, total_violations);

            msg.mtype = msg.modifyingGroup;
            if (msgsnd(moderator_msgid, &msg, sizeof(Message) - sizeof(long), 0) == -1) {
                fprintf(stderr, "Error in msgsnd: %s\n", strerror(errno));
                exit(1);
            }
        }
    }

    return 0;
}
