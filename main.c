#include "./avcodec.c"
#include <math.h>
#include <raylib.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#define MIN(a, b) (((a) < (b)) ? (a) : (b))
#define MAX(a, b) (((a) > (b)) ? (a) : (b))
#define MAX_FRAME_RATE 60
#define DIMENSION_SIZE 128

// using s16le which is 2 byte per sample
#define bytePerSample 2

#define PROGRESS_RED GetColor(0xFF5C5CFF)

const char *format_time(int seconds) {
    int hours = seconds / 3600;
    int minutes = (seconds / 60) % 60;
    seconds %= 60;

    if (hours == 0) {
        return TextFormat("%02d:%02d", minutes, seconds);
    } else {
        return TextFormat("%02d:%02d:%02d", hours, minutes, seconds);
    }
}

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

    // since frame rate comes in format like: 24000/1001
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

int ffaudio;  // lateinit in @main
int channels; // lateinit in @main
int total_frames = 0;
void AudioInputCallback(void *buffer, unsigned int frames) {
    ssize_t n = read(ffaudio, buffer, frames * channels * bytePerSample);
    if (n <= 0) {
        memset(buffer, 0, frames * channels * bytePerSample);
    } else {
        total_frames += n / (channels * bytePerSample);
    }
}

int main(int argc, char **argv) {
    if (argc < 2) {
        printf("cideo [FILE]\n");
        exit(EXIT_FAILURE);
    }
    char *const FILENAME = argv[1];
    if (fopen(FILENAME, "rb") == NULL) {
        perror("Couldn't open file");
        exit(EXIT_FAILURE);
    }

    setup_avcodec(FILENAME);

    int width = rgba->width;
    int height = rgba->height;
    AVRational framerate = video_stream->r_frame_rate;
    int frame_rate = ceil((float)framerate.num / framerate.den);
    float duration = (float)fmt_ctx->duration / AV_TIME_BASE;

    int sample_rate = 0;
    if (!noAudio) {
        channels = audio_dec_ctx->ch_layout.nb_channels;
        sample_rate = audio_dec_ctx->sample_rate;
    }

    const char *scale =
        TextFormat("fps=%d,scale=%d:%d", frame_rate, width, height);

    char *ffmpeg_command[] = {
        "ffmpeg", "-v",      "error",       "-loglevel", "quiet",    "-i",
        FILENAME, "-vf",     (char *)scale, "-f",        "rawvideo", "-pix_fmt",
        "rgba",   "-fflags", "+nobuffer",   "pipe:1",    NULL};

    int ffmpeg = fork_and_execute(ffmpeg_command, false);
    size_t frame_size = (1LL * width * height * 4);
    uint8_t *buf = malloc(sizeof(uint8_t) * frame_size);

    // audio
    if (!noAudio) {
        char *ffmpeg_audio_command[] = {
            "ffmpeg",  "-v",        "error",   "-loglevel",
            "quiet",   "-i",        FILENAME,  "-vn",
            "-f",      "s16le",     "-acodec", "pcm_s16le",
            "-fflags", "+nobuffer", "pipe:1",  NULL};

        ffaudio = fork_and_execute(ffmpeg_audio_command, false);
    }

    SetTraceLogLevel(LOG_ERROR);
    InitWindow(SCREEN_WIDTH, SCREEN_HEIGHT, FILENAME);
    SetTargetFPS(MIN(MAX_FRAME_RATE, frame_rate)); // clamp to MAX_FRAME_RATE

    InitAudioDevice();

    SetAudioStreamBufferSizeDefault(audio_dec_ctx->frame_size);

    AudioStream stream = {0};

    if (!noAudio) {
        stream = LoadAudioStream(sample_rate, 16, channels);
        SetAudioStreamCallback(stream, AudioInputCallback);
        SetAudioStreamVolume(stream, 0.5f);
        PlayAudioStream(stream);
    }

    Texture tex = LoadTextureFromImage(GenImageColor(width, height, BLACK));

    char formatted_duration[8] = {0};
    strcpy(formatted_duration, format_time(duration));

    const int frameX = (SCREEN_WIDTH - width) / 2;
    const int frameY = (SCREEN_HEIGHT - height) / 2;
    bool playing = true;
    bool ended = false;
    int frame_number = 0;
    while (!WindowShouldClose()) {
        if (IsKeyPressed(KEY_SPACE)) {
            playing = !playing;
            if (!playing) {
                PauseAudioStream(stream);
            } else {
                ResumeAudioStream(stream);
            }
        }

        if (ended && !noAudio) {
            StopAudioStream(stream);
        }

        const double current_time =
            MIN(duration, frame_number / (1.f * frame_rate));

        if (!ended && playing) {
            if (noAudio) {
                if (!extract_frame(ffmpeg, buf, frame_size)) {
                    ended = true;
                }
                UpdateTexture(tex, buf);
                frame_number++;
            } else {
                const double audio_time = (double)total_frames / sample_rate;
                const double diff = current_time - audio_time;
                const double thresh = 1.0 / frame_rate;
                if (diff < -thresh) {
                    // calculate the expected frame number and skip to in
                    // this way we match video with audio as the main clock

                    // int expected = audio_time * frame_rate;
                    // NOTE: Optionally round to nearest integer
                    int expected = (int)round(audio_time * frame_rate);
                    PauseAudioStream(stream);
                    for (int i = frame_number; i <= expected; i++) {
                        if (!extract_frame(ffmpeg, buf, frame_size)) {
                            ended = true;
                            break;
                        }
                    }
                    frame_number = expected;
                    ResumeAudioStream(stream);
                } else if (diff <= thresh) { // if within threshold, render
                    if (!extract_frame(ffmpeg, buf, frame_size)) {
                        ended = true;
                    }
                    UpdateTexture(tex, buf);
                    frame_number++;
                } // else { do nothing }
            }
        }

        BeginDrawing();

        ClearBackground(BLACK);
        DrawTexture(tex, frameX, frameY, WHITE);

        const float percent = current_time / duration * 100;

        // draw the progress bar
        const int lineThickness = 3;
        const int globRadius = 5;
        const int pad = 40;
        const int lineHeight = SCREEN_HEIGHT - pad;
        const int lineWidth = SCREEN_WIDTH;
        DrawLineEx((Vector2){0, lineHeight}, (Vector2){lineWidth, lineHeight},
                   lineThickness, GRAY);
        DrawLineEx((Vector2){0, lineHeight},
                   (Vector2){lineWidth * (percent / 100.f), lineHeight},
                   lineThickness, PROGRESS_RED);
        DrawCircle(lineWidth * (percent / 100), lineHeight, globRadius,
                   PROGRESS_RED);

        const char *time =
            TextFormat("%s/%s", format_time(current_time), formatted_duration);
        const int textPad =
            pad + 5 + MeasureTextEx(GetFontDefault(), time, 20, 0).y;
        DrawText(time, 10, SCREEN_HEIGHT - textPad, 20, PROGRESS_RED);

        EndDrawing();
    }

    free(buf);
    close(ffaudio);
    close(ffmpeg);
    UnloadTexture(tex);
    UnloadAudioStream(stream);
    CloseAudioDevice();
    CloseWindow();
    cleanup();
}
