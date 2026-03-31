#include <furi.h>
#include <furi_hal.h>
#include <gui/gui.h>
#include <input/input.h>
#include <notification/notification.h>
#include <notification/notification_messages.h>

#include <infrared.h>
#include <furi_hal_infrared.h>

#define TAG "FlipperLaserTag"

// Game parameters
#define FLT_STARTING_HP           5
#define FLT_SHOT_COMMAND          0x42
#define FLT_RX_SILENCE_TIMEOUT_US 60000U
#define FLT_HIT_COOLDOWN_MS       3000U

// Main loop tick interval (ms) — keeps hit-notify processing responsive
#define FLT_LOOP_TICK_MS 50U

typedef struct {
    FuriMessageQueue* input_queue;
    ViewPort* view_port;
    Gui* gui;
    NotificationApp* notifications;

    uint8_t hp;
    uint8_t player_id;       // derived from hardware UID
    const char* player_name; // device name from OTP

    bool shot_flash;
    bool hit_flash;

    // Set in ISR; cleared in main loop after sending notification
    volatile bool hit_notify_pending;
    // Tick value after which hits are accepted again
    volatile uint32_t cooldown_until_ticks;

    InfraredEncoderHandler* ir_encoder;
    InfraredDecoderHandler* ir_decoder;

    // Used by TX ISR
    InfraredMessage tx_msg;
} FlipperLaserTagApp;

// ---- Forward declarations ----
static void flt_viewport_draw_callback(Canvas* canvas, void* ctx);
static void flt_input_callback(InputEvent* event, void* ctx);
static void flt_handle_shot_received(FlipperLaserTagApp* app, const InfraredMessage* msg);

// ---------- IR TX: provide pulses to HAL ----------

typedef struct {
    InfraredEncoderHandler* encoder;
    bool last_sent;
} FlipperLtTxContext;

static FuriHalInfraredTxGetDataState
    flt_tx_get_data_isr(void* context, uint32_t* duration, bool* level) {
    FlipperLtTxContext* tx = context;

    if(tx->last_sent) {
        return FuriHalInfraredTxGetDataStateLastDone;
    }

    InfraredStatus status = infrared_encode(tx->encoder, duration, level);
    switch(status) {
    case InfraredStatusOk:
        return FuriHalInfraredTxGetDataStateOk;
    case InfraredStatusDone:
        tx->last_sent = true;
        return FuriHalInfraredTxGetDataStateDone;
    default:
        tx->last_sent = true;
        return FuriHalInfraredTxGetDataStateLastDone;
    }
}

// Send one IR "shot" frame
static void flt_send_shot(FlipperLaserTagApp* app) {
    if(furi_hal_infrared_is_busy()) {
        FURI_LOG_D(TAG, "IR busy, skipping shot");
        return;
    }

    FlipperLtTxContext tx_ctx = {
        .encoder = app->ir_encoder,
        .last_sent = false,
    };

    app->tx_msg.protocol = InfraredProtocolNEC;
    app->tx_msg.address = app->player_id;
    app->tx_msg.command = FLT_SHOT_COMMAND;
    app->tx_msg.repeat = false;

    infrared_reset_encoder(app->ir_encoder, &app->tx_msg);

    uint32_t freq = infrared_get_protocol_frequency(app->tx_msg.protocol);
    float duty = infrared_get_protocol_duty_cycle(app->tx_msg.protocol);

    FURI_LOG_D(TAG, "Sending shot: player_id=0x%02X cmd=0x%02X freq=%lu", app->player_id, FLT_SHOT_COMMAND, freq);

    furi_hal_infrared_set_tx_output(FuriHalInfraredTxPinInternal);
    furi_hal_infrared_async_tx_set_data_isr_callback(flt_tx_get_data_isr, &tx_ctx);
    furi_hal_infrared_async_tx_start(freq, duty);

    furi_hal_infrared_async_tx_wait_termination();
    furi_hal_infrared_async_tx_stop();

    app->shot_flash = true;
    FURI_LOG_I(TAG, "Shot fired");
}

// ---------- IR RX: capture and decode ----------

static void flt_rx_capture_cb(void* ctx, bool level, uint32_t duration) {
    FlipperLaserTagApp* app = ctx;
    const InfraredMessage* msg = infrared_decode(app->ir_decoder, level, duration);
    if(msg) {
        flt_handle_shot_received(app, msg);
    }
}

static void flt_rx_timeout_cb(void* ctx) {
    FlipperLaserTagApp* app = ctx;
    const InfraredMessage* msg = infrared_check_decoder_ready(app->ir_decoder);
    if(msg) {
        flt_handle_shot_received(app, msg);
    }
}

// Process a received IR frame (called from ISR context)
static void flt_handle_shot_received(FlipperLaserTagApp* app, const InfraredMessage* msg) {
    if(msg->protocol != InfraredProtocolNEC) {
        FURI_LOG_D(TAG, "Ignoring non-NEC IR frame (protocol=%d)", msg->protocol);
        return;
    }
    if(msg->command != FLT_SHOT_COMMAND) {
        FURI_LOG_D(TAG, "Ignoring unknown command 0x%02X", msg->command);
        return;
    }

    uint8_t shooter_id = (uint8_t)(msg->address & 0xFF);
    if(shooter_id == app->player_id) {
        FURI_LOG_D(TAG, "Ignoring own shot (id=0x%02X)", shooter_id);
        return;
    }

    // Ignore hits during cooldown
    if(furi_get_tick() < app->cooldown_until_ticks) {
        FURI_LOG_D(TAG, "Hit ignored — cooldown active (shooter=0x%02X)", shooter_id);
        return;
    }

    if(app->hp > 0) {
        app->hp--;
        app->hit_flash = true;
        app->hit_notify_pending = true;
        app->cooldown_until_ticks = furi_get_tick() + furi_ms_to_ticks(FLT_HIT_COOLDOWN_MS);
        FURI_LOG_I(TAG, "Hit by 0x%02X! HP: %u/%u", shooter_id, app->hp, FLT_STARTING_HP);
    } else {
        FURI_LOG_D(TAG, "Hit ignored — already dead (shooter=0x%02X)", shooter_id);
    }
}

// ---------- UI ----------

static void flt_viewport_draw_callback(Canvas* canvas, void* ctx) {
    FlipperLaserTagApp* app = ctx;

    canvas_clear(canvas);
    canvas_set_font(canvas, FontPrimary);
    canvas_draw_str(canvas, 4, 12, "Laser Tag");

    canvas_set_font(canvas, FontSecondary);
    char buf[32];

    snprintf(buf, sizeof(buf), "Player: %s", app->player_name ? app->player_name : "???");
    canvas_draw_str(canvas, 4, 24, buf);

    snprintf(buf, sizeof(buf), "HP: %u/%u", app->hp, FLT_STARTING_HP);
    canvas_draw_str(canvas, 4, 36, buf);

    if(app->shot_flash) {
        canvas_draw_str(canvas, 4, 56, "SHOT!");
        app->shot_flash = false;
    } else if(app->hit_flash) {
        canvas_draw_str(canvas, 4, 56, "HIT!");
        app->hit_flash = false;
    } else if(app->hp == 0) {
        canvas_draw_str(canvas, 4, 56, "GAME OVER");
    } else if(furi_get_tick() < app->cooldown_until_ticks) {
        canvas_draw_str(canvas, 4, 56, "COOLDOWN...");
    } else {
        canvas_draw_str(canvas, 4, 56, "OK: Shoot   BACK: Exit");
    }
}

static void flt_input_callback(InputEvent* event, void* ctx) {
    FlipperLaserTagApp* app = ctx;
    furi_message_queue_put(app->input_queue, event, FuriWaitForever);
}

// ---------- Main app ----------

int32_t flipper_laser_tag_app(void* p) {
    UNUSED(p);

    FlipperLaserTagApp app;

    // Use device name for display
    app.player_name = furi_hal_version_get_name_ptr();

    // Derive a consistent player ID from the hardware UID
    const uint8_t* uid = furi_hal_version_uid();
    size_t uid_size = furi_hal_version_uid_size();
    uint8_t derived_id = 0;
    for(size_t i = 0; i < uid_size; i++) derived_id ^= uid[i];
    if(derived_id == 0) derived_id = 1;
    app.player_id = derived_id;

    app.hp = FLT_STARTING_HP;
    app.shot_flash = false;
    app.hit_flash = false;
    app.hit_notify_pending = false;
    app.cooldown_until_ticks = 0;

    FURI_LOG_I(TAG, "Starting: player=%s id=0x%02X hp=%u", app.player_name ? app.player_name : "???", app.player_id, app.hp);

    // Allocate IR encoder/decoder
    app.ir_encoder = infrared_alloc_encoder();
    app.ir_decoder = infrared_alloc_decoder();

    // Start RX
    furi_hal_infrared_async_rx_start();
    furi_hal_infrared_async_rx_set_timeout(FLT_RX_SILENCE_TIMEOUT_US);
    furi_hal_infrared_async_rx_set_capture_isr_callback(flt_rx_capture_cb, &app);
    furi_hal_infrared_async_rx_set_timeout_isr_callback(flt_rx_timeout_cb, &app);

    // Open notification service
    app.notifications = furi_record_open(RECORD_NOTIFICATION);

    // GUI setup
    app.input_queue = furi_message_queue_alloc(8, sizeof(InputEvent));
    app.view_port = view_port_alloc();
    view_port_enabled_set(app.view_port, true);
    view_port_draw_callback_set(app.view_port, flt_viewport_draw_callback, &app);
    view_port_input_callback_set(app.view_port, flt_input_callback, &app);

    app.gui = furi_record_open(RECORD_GUI);
    gui_add_view_port(app.gui, app.view_port, GuiLayerFullscreen);

    FURI_LOG_D(TAG, "IR RX started, GUI ready, entering main loop");

    // Main loop — uses a timeout so hit notifications are processed promptly
    bool running = true;
    while(running) {
        InputEvent event;
        FuriStatus status =
            furi_message_queue_get(app.input_queue, &event, furi_ms_to_ticks(FLT_LOOP_TICK_MS));

        if(status == FuriStatusOk) {
            if(event.type == InputTypeShort && event.key == InputKeyOk) {
                bool in_cooldown = furi_get_tick() < app.cooldown_until_ticks;
                if(app.hp > 0 && !in_cooldown) {
                    flt_send_shot(&app);
                    notification_message(app.notifications, &sequence_success);
                } else {
                    FURI_LOG_D(TAG, "Shot blocked: hp=%u cooldown=%s", app.hp, in_cooldown ? "yes" : "no");
                }
            } else if(event.type == InputTypeShort && event.key == InputKeyBack) {
                FURI_LOG_I(TAG, "Back pressed, exiting");
                running = false;
            }
        }

        // Process hit notification queued from ISR
        if(app.hit_notify_pending) {
            app.hit_notify_pending = false;
            notification_message(app.notifications, &sequence_error);
            if(app.hp == 0) {
                FURI_LOG_I(TAG, "Player eliminated");
            }
        }
    }

    // Cleanup
    FURI_LOG_D(TAG, "Cleaning up");

    gui_remove_view_port(app.gui, app.view_port);
    furi_record_close(RECORD_GUI);

    view_port_free(app.view_port);
    furi_message_queue_free(app.input_queue);

    furi_hal_infrared_async_rx_stop();

    infrared_free_encoder(app.ir_encoder);
    infrared_free_decoder(app.ir_decoder);

    furi_record_close(RECORD_NOTIFICATION);

    FURI_LOG_I(TAG, "Exited cleanly");

    return 0;
}
