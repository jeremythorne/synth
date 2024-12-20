#include <memory>
#include <mutex>
#include <thread>
#include <vector>
#include <SDL2/SDL.h>

constexpr size_t buffer_size = 1024;
constexpr unsigned samples_per_sec = 44100;

const char* getError() {
    return SDL_GetError();
}

struct Synth {
    uint64_t t = 0;

    struct Sequencer {
        int bpm = 138 * 4;
        float pattern[8] = {440, 0,  698.5, 400, 554.4, 698.5, 830.6, 554.4};
        size_t sample = 0;
        float tick(size_t count) {
            auto beat_length = 60 * samples_per_sec / bpm;
            auto pattern_length = beat_length * 8;
            auto note = pattern[sample / beat_length];
            sample = (sample + count) % pattern_length;
            return note;
        }
    } sequencer;

    struct SawTooth {
        std::atomic<float> tuning_v = 1.0f;
        float tuning = 1.0;
        float volume = 0.25;
        float last = 0;
        void tick(float note, int16_t *data, size_t count) {
            float value = last;
            float delta = 0;
            if (note) {
                tuning = std::clamp(tuning * tuning_v, 0.1f, 1000.0f);
                auto freq = std::clamp(tuning * note, 10.0f, 10000.0f);
                auto period = samples_per_sec / freq;
                delta = 2.0 / period;
                value = last + delta;
                if (value > 1.0) { value -= 2.0; }
            }
            auto scale = SHRT_MAX;
            for (size_t i = 0; i < count; i++) {
                data[i] = value * volume * scale;
                value += delta;
                if (value > 1.0) { value -= 2.0; }
            }
            last = value;
        }
    } sawtooth;

    struct LowPass {
        std::atomic<float> rc_v = 1.0;
        float rc = 0.5;
        float value[4] = {0, 0, 0, 0};
        void tick(int16_t *data, size_t count) {
            rc = std::clamp(rc * rc_v, 0.0f, 1.0f);
            for (int j = 0; j < 4; j++) {
                for (size_t i = 0; i < count; i++) {
                    value[j] = std::clamp(data[i] * rc + value[j] * (1.0 - rc),
                        static_cast<double>(SHRT_MIN), static_cast<double>(SHRT_MAX));
                    data[i] = value[j];
                }
            }
        }
    } lowpass;

    void make_sound(int16_t *data, size_t count) {
        auto note = sequencer.tick(count);
        sawtooth.tick(note, data, count);
        lowpass.tick(data, count);
    }

    void tuning(int a) {
        sawtooth.tuning_v = 1.0 + 0.01 * a;
    }

    void cutoff(int a) {
        lowpass.rc_v = 1.0 + 0.01 * a;
    }
};

struct CircularBuffer {
    std::vector<int16_t> samples;
    std::atomic<size_t> write_a = 0;
    std::atomic<size_t> read_a = 0;
    bool first = true;

    CircularBuffer(size_t size) :
        samples(size) {}

    size_t copy_out(int16_t *dest, size_t count) {
        int16_t *src = samples.data();
        size_t count_out = 0;
        size_t read = read_a;
        size_t write = write_a;
        if (read < write) {
            count_out = std::min(count, write - read);
            std::copy(src + read,
                    src + read + count_out, dest);
        } else {
            auto size1 = std::min(samples.size() - read, count);
            auto size2 = std::max(0ul, std::min(count - size1, write));
            std::copy(src + read, src + read + size1, dest);
            std::copy(src, src + size2, dest + size1);
            count_out = size1 + size2;
        }
        read_a = (read + count_out) % samples.size();
        //printf("r%zu", read);
        return count - count_out;
    }

    size_t copy_in(int16_t *src, size_t count) {
        int16_t *dest = samples.data();
        size_t count_in = 0;
        size_t read = read_a;
        size_t write = write_a;
        if (first) {
            count_in = std::min(count, samples.size());
            std::copy(src, src + count_in, dest + write);
            first = false;
        }
        else if (read > write) {
            count_in = std::min(count, read - write);
            std::copy(src, src + count_in, dest + write);
        } else if (read < write) {
            auto size1 = std::min(samples.size() - write, count);
            auto size2 = std::max(0ul, std::min(count - size1, read));
            std::copy(src, src + size1, dest + write);
            std::copy(src + size1, src + size1 + size2, dest);
            count_in = size1 + size2;
        }
        write_a = (write + count_in) % samples.size();
        return count - count_in;
    }

    bool has_space() {
        return write_a != read_a;
    }
};

struct Audio {
    Audio():
        buffer(buffer_size * 2) {}

    void play();

    void tuning(int a) {
        synth.tuning(a);
    }

    void cutoff(int a) {
        synth.cutoff(a);
    }

    SDL_AudioDeviceID dev = 0;
    Synth synth;

    CircularBuffer buffer;
    bool quit = false;
    std::condition_variable cv;
    std::mutex mutex;

    std::thread thread;
    void notify() {
        cv.notify_one();
    }
    bool start();
    ~Audio();
};

void audioCallback(void * userdata, uint8_t *stream, int32_t len) {
    if (!userdata || !stream) {
        return;
    }

    //printf("c");

    auto &audio = *reinterpret_cast<Audio *>(userdata);
    auto count = len / sizeof(int16_t);
    auto buffer = reinterpret_cast<int16_t *>(stream);

    auto left = audio.buffer.copy_out(buffer, count);
    audio.notify();
    if (left) {
        //printf("%zu", left);
        std::fill(buffer + count - left, buffer + count, 0);
    }

    //audio.synth.make_sound(buffer, count);
}

bool Audio::start() {
    auto have = SDL_AudioSpec();
    auto want = SDL_AudioSpec();
    want.freq = samples_per_sec;
    want.format = AUDIO_S16LSB;
    want.channels = 1;
    want.samples = buffer_size;
    want.callback = audioCallback;
    want.userdata = this;
    dev = SDL_OpenAudioDevice(nullptr, 0, &want, &have, 0);
    if (dev == 0) {
        printf("couldn't open audio device\n");
        return false;
    }
    SDL_PauseAudioDevice(dev, 0);
    return true;
}

Audio::~Audio() {
    if (thread.joinable()) {
        {
            std::lock_guard lock(mutex);
            quit = true;
        }
        notify();
        thread.join();
    }
    SDL_CloseAudioDevice(dev);
}

void Audio::play() {
    if (thread.joinable()) {
        return;
    }
    thread = std::thread([this]() {
        bool should_quit = false;
        while(!should_quit) {
            //printf("t");
            std::array<int16_t, buffer_size> data;

            synth.make_sound(data.data(), data.size());

            auto left = buffer.copy_in(data.data(), data.size());
            if (left) {
                std::unique_lock lock(mutex);
                //printf("w\n");
                cv.wait(lock, [this](){ return quit || buffer.has_space(); });
                if (!quit) {
                    buffer.copy_in(data.data() + data.size() - left, left); 
                }
            }
            should_quit = quit;
        }
    });
}

struct Keyboard {
    const uint8_t* state = nullptr;
    int32_t length = 0;
    Keyboard() {
        state = SDL_GetKeyboardState(&length);
    }

    void update() {
        SDL_PumpEvents();
    }

    bool pressed(SDL_Scancode key) {
        if (key < 0 || key > length) {
            return false;
        }
        return state[key] == 1;
    }
};

struct Window {
    SDL_Window *window = nullptr;
    Window(int width, int height) {
        window = SDL_CreateWindow("hello",
                                       0x2FFF0000,//SDL_WINDOWPOS_CENTERED,
                                       0x2FFF0000,//SDL_WINDOWPOS_CENTERED,
                                       width,
                                       height,
                                       0);
        if (!window) {
            printf("couldn't create window\n");
        }
    }
/*
    func createRenderer() throws -> Renderer {
        return try Renderer(window:self.window)
    }
*/
    ~Window() {
        if (window) {
            SDL_DestroyWindow(window);
        }
    }
};

struct Event {
    SDL_Event event;

    Event(SDL_Event sdl_event) :
            event(sdl_event) {}
};


struct SDL {
    ~SDL() {
        SDL_Quit();
    }

    bool init() {
        return SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO) >= 0;
    }

    std::shared_ptr<Window> createWindow(int width, int height) {
        auto window = std::make_shared<Window>(width, height);
        if (!window->window) {
            return {};
        }
        return window;
    }

    std::shared_ptr<Audio> createAudio() {
        auto audio = std::make_shared<Audio>();
        if (!audio->start()) {
            return {};
        }
        return audio;
    }

    std::shared_ptr<Keyboard> createKeyboard() {
        auto keyboard = std::make_shared<Keyboard>();
        if (!keyboard->length) {
            return {};
        }
        return keyboard;
    }

    std::optional<Event> pollEvent() {
        auto sdl_event = SDL_Event();
        if (SDL_PollEvent(&sdl_event) == 0) {
            return {};
        }
        return Event(sdl_event);
    }

    uint32_t time() {
        return SDL_GetTicks();
    }

    void sleep(uint32_t ms) {
        SDL_Delay(ms);
    }
};

int main() {
    SDL sdl;
    sdl.init();
    auto window = sdl.createWindow(100, 100);
    auto audio = sdl.createAudio();
    auto keyboard = sdl.createKeyboard();
    bool shouldQuit = false;
    audio->play();

    while (!shouldQuit) {
        while (auto event = sdl.pollEvent()) {
            switch (event->event.type) {
            case SDL_QUIT:
                shouldQuit = true;
                break;
            }
        }
        if (keyboard->pressed(SDL_SCANCODE_ESCAPE)) {
            shouldQuit = true;
        }
        if (keyboard->pressed(SDL_SCANCODE_UP)) {
            audio->tuning(1);
        } else if(keyboard->pressed(SDL_SCANCODE_DOWN)) {
            audio->tuning(-1);
        } else {
            audio->tuning(0);
        }

        if (keyboard->pressed(SDL_SCANCODE_LEFT)) {
            audio->cutoff(1);
        } else if(keyboard->pressed(SDL_SCANCODE_RIGHT)) {
            audio->cutoff(-1);
        } else {
            audio->cutoff(0);
        }

    }

    return 0;
}