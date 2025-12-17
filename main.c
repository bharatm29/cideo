#include <math.h>
#include <raylib.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

// modifies buf
ssize_t extract_frame(int fd, uint8_t *buf, size_t frame_size) {
    size_t total = 0;
    while (total < frame_size) {
        ssize_t n = read(fd, buf + total, frame_size - total);
        if (n <= 0) {
            return n; // EOF
        }
        total += n;
    }
    // printf("read: %lu\n", total);
    return total;
}

int fork_and_execute(char **command, bool wait) {
    int fd[2];
    if (pipe(fd) == -1) { // Setup the pipe
        perror("pipe");
        exit(EXIT_FAILURE);
    }

    pid_t cpid;

    cpid = fork();
    if (cpid == -1) {
        perror("fork");
        exit(EXIT_FAILURE);
    }

    if (cpid == 0) {
        close(fd[0]);               // child doesn't read
        dup2(fd[1], STDOUT_FILENO); // stdout -> child's write end
        close(fd[1]);

        execvp(command[0], command);
        perror("execvp");

        exit(1);
    }
    close(fd[1]);

    if (wait) {
        int wstatus;
        waitpid(cpid, &wstatus, 0);

        if (!WIFEXITED(wstatus) && !WIFSIGNALED(wstatus)) {
            perror("waitpid");
            exit(EXIT_FAILURE);
        }
    }

    return fd[0];
}

int get_frame_rate(char *fstr) {
    char *cur = fstr;

    // since frame rate comes in format like: 60/1
    while (*++cur != '/')
        ;

    *cur = '\0';

    return ceil(strtod(fstr, NULL));
}

int main(int argc, char **argv) {
    if (argc != 2) {
        printf("cideo [FILE]\n");
        exit(EXIT_FAILURE);
    }
    char *const FILENAME = argv[1];

    char *dimensions_command[] = {"ffprobe",
                                  "-v",
                                  "error",
                                  "-select_streams",
                                  "v:0",
                                  "-show_entries",
                                  "stream=width,height,r_frame_rate,duration",
                                  "-of",
                                  "csv=p=0",
                                  FILENAME,
                                  NULL};

    int ffprobe = fork_and_execute(dimensions_command, true);
    uint8_t dimension[64] = {0}; // expecting maximum to be 64 bytes for now
    ssize_t n;
    if (!(n = read(ffprobe, dimension, 64))) {
        perror("read");
        exit(EXIT_FAILURE);
    }
    close(ffprobe);

    dimension[n - 1] = '\0'; // dimension[n] will be a \n, thus we skip that

    // format:- 1920,1200,60/1,29.699995
    int width = strtol(strtok((char *)dimension, ","), NULL, 10);
    int height = strtol(strtok(NULL, ","), NULL, 10);
    int frame_rate = get_frame_rate(strtok(NULL, ","));
    float duration = strtod(strtok(NULL, ","), NULL);

    char *ffmpeg_command[] = {
        "ffmpeg", "-v",       "error",    "-loglevel", "quiet",  "-i", FILENAME,
        "-f",     "rawvideo", "-pix_fmt", "rgba",      "pipe:1", NULL};

    int ffmpeg = fork_and_execute(ffmpeg_command, false);

    size_t frame_size = (1LL * width * height * 4);
    uint8_t *buf = malloc(sizeof(uint8_t) * frame_size);

    SetTraceLogLevel(LOG_ERROR);
    InitWindow(width, height, "Frames");
    SetTargetFPS(frame_rate);

    Texture tex = LoadTextureFromImage(GenImageColor(width, height, BLACK));
    UpdateTexture(tex, buf);

    bool playing = true;
    bool ended = false;
    uint frame_number = 0;
    while (!WindowShouldClose()) {
        if (IsKeyPressed(KEY_SPACE)) {
            playing = !playing;
        }

        if (!ended && playing) {
            if (!extract_frame(ffmpeg, buf, frame_size)) {
                ended = true;
            }
            frame_number++;
            UpdateTexture(tex, buf);
        }

        BeginDrawing();
        ClearBackground(BLACK);
        DrawTexture(tex, 0, 0, WHITE);
        float current_time = frame_number / (1.f * frame_rate);
        if (current_time > duration)
            current_time = duration; // clamping just in case

        float percent = current_time / duration * 100;
        int pad = 40;
        int lineHeight = height - pad;
        DrawLineEx((Vector2){0, lineHeight}, (Vector2){width, lineHeight}, 6.0f,
                   GRAY);
        DrawLineEx((Vector2){0, lineHeight},
                   (Vector2){width * (percent / 100), lineHeight}, 6.0f, RED);
        DrawCircle(width * (percent / 100), lineHeight, 5, RED);

        const char *time = TextFormat("%.2f/%.2f", current_time, duration);
        int textPad = pad + 5 + MeasureTextEx(GetFontDefault(), time, 20, 0).y;
        DrawText(time, 10, height - textPad, 20, RED);

        EndDrawing();
    }

    free(buf);
    close(ffmpeg);
    UnloadTexture(tex);
    CloseWindow();
}
