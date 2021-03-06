#include "controller.h"

#include "lockutil.h"
#include "log.h"

SDL_bool controller_init(struct controller *controller, socket_t video_socket) {
    if (!control_event_queue_init(&controller->queue)) {
        return SDL_FALSE;
    }

    if (!(controller->mutex = SDL_CreateMutex())) {
        return SDL_FALSE;
    }

    if (!(controller->event_cond = SDL_CreateCond())) {
        SDL_DestroyMutex(controller->mutex);
        return SDL_FALSE;
    }

    controller->video_socket = video_socket;
    controller->stopped = SDL_FALSE;

    return SDL_TRUE;
}

void controller_destroy(struct controller *controller) {
    SDL_DestroyCond(controller->event_cond);
    SDL_DestroyMutex(controller->mutex);
    control_event_queue_destroy(&controller->queue);
}

SDL_bool controller_push_event(struct controller *controller, const struct control_event *event) {
    SDL_bool res;
    mutex_lock(controller->mutex);
    SDL_bool was_empty = control_event_queue_is_empty(&controller->queue);
    res = control_event_queue_push(&controller->queue, event);
    if (was_empty) {
        cond_signal(controller->event_cond);
    }
    mutex_unlock(controller->mutex);
    return res;
}

static SDL_bool process_event(struct controller *controller, const struct control_event *event) {
    unsigned char serialized_event[SERIALIZED_EVENT_MAX_SIZE];
    int length = control_event_serialize(event, serialized_event);
    if (!length) {
        return SDL_FALSE;
    }
    int w = net_send_all(controller->video_socket, serialized_event, length);
    return w == length;
}

static int run_controller(void *data) {
    struct controller *controller = data;

    mutex_lock(controller->mutex);
    for (;;) {
        while (!controller->stopped && control_event_queue_is_empty(&controller->queue)) {
            cond_wait(controller->event_cond, controller->mutex);
        }
        if (controller->stopped) {
            // stop immediately, do not process further events
            break;
        }
        struct control_event event;
        while (control_event_queue_take(&controller->queue, &event)) {
            SDL_bool ok = process_event(controller, &event);
            control_event_destroy(&event);
            if (!ok) {
                LOGD("Cannot write event to socket");
                goto end;
            }
        }
    }
end:
    mutex_unlock(controller->mutex);
    return 0;
}

SDL_bool controller_start(struct controller *controller) {
    LOGD("Starting controller thread");

    controller->thread = SDL_CreateThread(run_controller, "controller", controller);
    if (!controller->thread) {
        LOGC("Could not start controller thread");
        return SDL_FALSE;
    }

    return SDL_TRUE;
}

void controller_stop(struct controller *controller) {
    mutex_lock(controller->mutex);
    controller->stopped = SDL_TRUE;
    cond_signal(controller->event_cond);
    mutex_unlock(controller->mutex);
}

void controller_join(struct controller *controller) {
    SDL_WaitThread(controller->thread, NULL);
}
