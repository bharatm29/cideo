#include <math.h>
#include <raylib.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#define MIN(a, b) (((a) < (b)) ? (a) : (b))
#define MAX(a, b) (((a) > (b)) ? (a) : (b))
#define SCREEN_WIDTH 1920
#define SCREEN_HEIGHT 1200
#define MAX_FRAME_RATE 60
#define MAX_SAMPLES_PER_UPDATE 4096

#define PROGRESS_RED GetColor(0xFF5C5CFF)

typedef unsigned char uint8_t; // #include <stdint.h>

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

    // since frame rate comes in format like: 24000/1001 =>
    int a = 0;
    int b = 0;

    while (*cur != '/') {
        cur++;
    }

    *cur = '\0';
    a = strtol(fstr, NULL, 10);
    b = strtol(++cur, NULL, 10);

    return ceil(a / (1.f * b));
}

int ffaudio;
int channels;
void AudioInputCallback(void *buffer, unsigned int frames) {
    ssize_t n = read(ffaudio, buffer, frames * channels * 2);
    if (n <= 0) {
        memset(buffer, 0, frames * channels * 2);
    }
}

int main(int argc, char **argv) {
    if (argc != 2) {
        printf("cideo [FILE]\n");
        exit(EXIT_FAILURE);
    }
    char *const FILENAME = argv[1];

    // audio
    int sample_rate = 44100;
    channels = 2;
    int bytePerSample = 2; // using s16le

    char *ffmpeg_audio_command[] = {
        "ffmpeg", "-v",    "error",   "-loglevel", "quiet", "-i", FILENAME,
        "-vn", // disable video
        "-f",     "s16le", "-acodec", "pcm_s16le", "pipe:1"};

    ffaudio = fork_and_execute(ffmpeg_audio_command, false);
    ssize_t audioBytes =
        sizeof(uint8_t) * 2 * channels * MAX_SAMPLES_PER_UPDATE;
    uint8_t *audio_buf = malloc(audioBytes);

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

    // clamp width to fit to screen
    float scaleFactor =
        MIN((float)SCREEN_WIDTH / width, (float)SCREEN_HEIGHT / height);

    // NOTE: following allows cropping to fill screen without keeping aspect
    // ratio
    // width = MIN(SCREEN_WIDTH, width * SCREEN_HEIGHT / height);
    // height = MIN(SCREEN_HEIGHT, height * SCREEN_WIDTH / width);

    // scales it to ensure it fits in screen width and height while keeping
    // aspect ratio
    width *= scaleFactor;
    height *= scaleFactor;

    const char *scale = TextFormat("scale=%d:%d", width, height);

    char *ffmpeg_command[] = {"ffmpeg",      "-v",     "error",    "-loglevel",
                              "quiet",       "-i",     FILENAME,   "-vf",
                              (char *)scale, "-f",     "rawvideo", "-pix_fmt",
                              "rgba",        "pipe:1", NULL};

    int ffmpeg = fork_and_execute(ffmpeg_command, false);
    size_t frame_size = (1LL * width * height * 4);
    uint8_t *buf = malloc(sizeof(uint8_t) * frame_size);


    InitWindow(width, height, FILENAME);
    SetTargetFPS(MIN(MAX_FRAME_RATE, frame_rate)); // clamp to MAX_FRAME_RATE

    InitAudioDevice();

    SetAudioStreamBufferSizeDefault(MAX_SAMPLES_PER_UPDATE);

    AudioStream stream = LoadAudioStream(sample_rate, 16, channels);
    SetAudioStreamCallback(stream, AudioInputCallback);
    SetAudioStreamVolume(stream, 0.5f);
    PlayAudioStream(stream);

    Texture tex = LoadTextureFromImage(GenImageColor(width, height, BLACK));

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

        DrawTexture(tex, 0, 0, WHITE);

        const float current_time =
            MIN(duration, frame_number / (1.f * frame_rate));
        const float percent = current_time / duration * 100;

        // draw the progress bar
        const int lineThickness = 3;
        const int globRadius = 5;
        const int pad = 40;
        const int lineHeight = height - pad;
        DrawLineEx((Vector2){0, lineHeight}, (Vector2){width, lineHeight},
                   lineThickness, GRAY);
        DrawLineEx((Vector2){0, lineHeight},
                   (Vector2){width * (percent / 100), lineHeight},
                   lineThickness, PROGRESS_RED);
        DrawCircle(width * (percent / 100), lineHeight, globRadius,
                   PROGRESS_RED);

        const char *time = TextFormat("%.2f/%.2f", current_time, duration);
        const int textPad =
            pad + 5 + MeasureTextEx(GetFontDefault(), time, 20, 0).y;
        DrawText(time, 10, height - textPad, 20, PROGRESS_RED);

        EndDrawing();
    }

    free(buf);
    close(ffaudio);
    close(ffmpeg);
    UnloadTexture(tex);
    UnloadAudioStream(stream);
    CloseAudioDevice();
    CloseWindow();
}
