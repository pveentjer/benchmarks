/*
 * Copyright 2015-2025 Real Logic Limited.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 * https://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <signal.h>
#include <inttypes.h>
#include <sched.h>

#include "aeronc.h"
#include "aeron_agent.h"
#include "aeron_archive.h"
#include "aeron_archive_persistent_subscription.h"
#include "uri/aeron_uri_string_builder.h"

#define TIMESTAMP_OFFSET        0
#define RECEIVER_INDEX_OFFSET   (TIMESTAMP_OFFSET + 8)
#define PROCESSING_TIME_OFFSET  (RECEIVER_INDEX_OFFSET + 4)
#define MESSAGE_ID_OFFSET       (PROCESSING_TIME_OFFSET + 8)

#define FRAME_HEADER_FLAGS_OFFSET  5

#define FRAGMENT_LIMIT_DEFAULT          10
#define CONNECTION_TIMEOUT_NS_DEFAULT   (60LL * 1000000000LL)
#define PROPERTIES_MAX_LINE             4096
#define PROPERTIES_MAX_KEY              512
#define PROPERTIES_MAX_VALUE            2048
#define PROPERTIES_MAX_ENTRIES          256

#define MIX_CONSTANT_1  UINT64_C(0xff51afd7ed558ccd)
#define MIX_CONSTANT_2  UINT64_C(0xc4ceb9fe1a85ec53)

static inline uint64_t murmur3_checksum(uint64_t h)
{
    h ^= h >> 33;
    h *= MIX_CONSTANT_1;
    h ^= h >> 33;
    h *= MIX_CONSTANT_2;
    h ^= h >> 33;
    return h;
}

typedef struct
{
    char key[PROPERTIES_MAX_KEY];
    char value[PROPERTIES_MAX_VALUE];
} property_entry_t;

typedef struct
{
    property_entry_t entries[PROPERTIES_MAX_ENTRIES];
    int count;
} properties_t;

static int properties_load(properties_t *props, const char *filename)
{
    FILE *f = fopen(filename, "r");
    if (!f)
    {
        fprintf(stderr, "Failed to open properties file: %s\n", filename);
        return -1;
    }

    char line[PROPERTIES_MAX_LINE];
    while (fgets(line, sizeof(line), f))
    {
        size_t len = strlen(line);
        while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r'))
        {
            line[--len] = '\0';
        }

        if (len == 0 || line[0] == '#' || line[0] == '!')
        {
            continue;
        }

        char *eq = strchr(line, '=');
        if (!eq)
        {
            continue;
        }

        if (props->count >= PROPERTIES_MAX_ENTRIES)
        {
            fprintf(stderr, "Too many properties (max %d)\n", PROPERTIES_MAX_ENTRIES);
            fclose(f);
            return -1;
        }

        size_t key_len = (size_t)(eq - line);
        if (key_len >= PROPERTIES_MAX_KEY)
        {
            continue;
        }

        property_entry_t *e = &props->entries[props->count++];
        strncpy(e->key, line, key_len);
        e->key[key_len] = '\0';
        strncpy(e->value, eq + 1, PROPERTIES_MAX_VALUE - 1);
        e->value[PROPERTIES_MAX_VALUE - 1] = '\0';
    }

    fclose(f);
    return 0;
}

static void properties_merge(properties_t *dst, const properties_t *src)
{
    for (int j = 0; j < src->count; j++)
    {
        bool found = false;
        for (int k = 0; k < dst->count; k++)
        {
            if (strcmp(dst->entries[k].key, src->entries[j].key) == 0)
            {
                strncpy(dst->entries[k].value, src->entries[j].value, PROPERTIES_MAX_VALUE - 1);
                found = true;
                break;
            }
        }
        if (!found && dst->count < PROPERTIES_MAX_ENTRIES)
        {
            dst->entries[dst->count++] = src->entries[j];
        }
    }
}

static const char *properties_get(const properties_t *props, const char *key, const char *default_value)
{
    for (int i = 0; i < props->count; i++)
    {
        if (strcmp(props->entries[i].key, key) == 0)
        {
            return props->entries[i].value;
        }
    }
    return default_value;
}

static int64_t properties_get_int64(const properties_t *props, const char *key, int64_t default_value)
{
    const char *v = properties_get(props, key, NULL);
    return v ? (int64_t)strtoll(v, NULL, 10) : default_value;
}

static int32_t properties_get_int32(const properties_t *props, const char *key, int32_t default_value)
{
    const char *v = properties_get(props, key, NULL);
    return v ? (int32_t)strtol(v, NULL, 10) : default_value;
}

static inline int32_t read_int32_le(const uint8_t *buf, size_t offset)
{
    return (int32_t)(
        (uint32_t)buf[offset]           |
        ((uint32_t)buf[offset + 1] << 8)  |
        ((uint32_t)buf[offset + 2] << 16) |
        ((uint32_t)buf[offset + 3] << 24));
}

static inline int64_t read_int64_le(const uint8_t *buf, size_t offset)
{
    return (int64_t)(
        (uint64_t)buf[offset]           |
        ((uint64_t)buf[offset + 1] << 8)  |
        ((uint64_t)buf[offset + 2] << 16) |
        ((uint64_t)buf[offset + 3] << 24) |
        ((uint64_t)buf[offset + 4] << 32) |
        ((uint64_t)buf[offset + 5] << 40) |
        ((uint64_t)buf[offset + 6] << 48) |
        ((uint64_t)buf[offset + 7] << 56));
}

static aeron_subscription_t *add_subscription(
    aeron_t *aeron,
    const char *channel,
    int32_t stream_id)
{
    aeron_async_add_subscription_t *async = NULL;
    if (aeron_async_add_subscription(&async, aeron, channel, stream_id, NULL, NULL, NULL, NULL) < 0)
    {
        fprintf(stderr, "aeron_async_add_subscription failed: %s\n", aeron_errmsg());
        return NULL;
    }

    aeron_subscription_t *sub = NULL;
    int result;
    while ((result = aeron_async_add_subscription_poll(&sub, async)) == 0)
    {
        sched_yield();
    }

    if (result < 0)
    {
        fprintf(stderr, "aeron_async_add_subscription_poll failed: %s\n", aeron_errmsg());
        return NULL;
    }

    return sub;
}

typedef struct
{
    aeron_exclusive_publication_t *publication;
    int32_t receiver_index;
    volatile bool *running;
    int64_t requests_received;
    int64_t responses_sent;
    uint64_t running_checksum;
} echo_context_t;

static void on_fragment(void *clientd, const uint8_t *buffer, size_t length, aeron_header_t *header)
{
    echo_context_t *ctx = (echo_context_t *)clientd;

    if (read_int32_le(buffer, RECEIVER_INDEX_OFFSET) != ctx->receiver_index)
    {
        return;
    }

    ctx->requests_received++;

    aeron_buffer_claim_t claim;
    int64_t result;

    while ((result = aeron_exclusive_publication_try_claim(ctx->publication, length, &claim)) <= 0)
    {
        if (result == AERON_PUBLICATION_CLOSED || result == AERON_PUBLICATION_ERROR)
        {
            fprintf(stderr, "Publication error during try_claim: %" PRId64 "\n", result);
            return;
        }
        if (!*ctx->running)
        {
            return;
        }
    }

    aeron_header_values_t hv;
    aeron_header_values(header, &hv);

    memcpy(claim.data, buffer, length);
    claim.frame_header[FRAME_HEADER_FLAGS_OFFSET] = hv.frame.flags;

    aeron_buffer_claim_commit(&claim);

    ctx->responses_sent++;

    const int64_t message_id = read_int64_le(buffer, MESSAGE_ID_OFFSET);
    ctx->running_checksum = ((ctx->running_checksum << 1) | (ctx->running_checksum >> 63))
                            ^ murmur3_checksum((uint64_t)message_id);

    const int64_t processing_time_ns = read_int64_le(buffer, PROCESSING_TIME_OFFSET);
    if (processing_time_ns > 0)
    {
        const int64_t spin_start = aeron_nano_clock();
        while (aeron_nano_clock() - spin_start < processing_time_ns)
        {
            /* spin */
        }
    }
}

static aeron_controlled_fragment_handler_action_t on_fragment_controlled(
    void *clientd, const uint8_t *buffer, size_t length, aeron_header_t *header)
{
    on_fragment(clientd, buffer, length, header);
    return AERON_ACTION_CONTINUE;
}

typedef struct echo_state_stct echo_state_t;

struct echo_state_stct
{
    void (*await_connected)(echo_state_t *state);
    int  (*poll)(echo_state_t *state);
    void (*close)(echo_state_t *state);
};

typedef struct
{
    echo_state_t base;

    aeron_t *aeron;
    echo_context_t *echo_ctx;

    aeron_subscription_t *subscription;
    aeron_image_t *image;

    const char *channel;
    int32_t stream_id;
    int64_t connection_timeout_ns;

    bool live;
} gap_state_t;

static void gap_await_connected(echo_state_t *base)
{
    gap_state_t *s = (gap_state_t *)base;
    const int64_t deadline_ns = aeron_nano_clock() + s->connection_timeout_ns;

    while (!(aeron_subscription_is_connected(s->subscription) &&
             aeron_exclusive_publication_is_connected(s->echo_ctx->publication)))
    {
        if (aeron_nano_clock() > deadline_ns)
        {
            fprintf(stderr, "GapState: timeout waiting for connection\n");
            exit(1);
        }
        sched_yield();
    }

    s->image = aeron_subscription_image_at_index(s->subscription, 0);
    s->live  = true;
}

static int gap_poll(echo_state_t *base)
{
    gap_state_t *s = (gap_state_t *)base;

    if (s->live)
    {
        if (aeron_image_is_closed(s->image))
        {
            aeron_subscription_close(s->subscription, NULL, NULL);
            s->subscription = NULL;
            s->image        = NULL;
            s->live         = false;
            return 1;
        }
        return aeron_image_poll(s->image, on_fragment, s->echo_ctx, FRAGMENT_LIMIT_DEFAULT);
    }
    else
    {
        if (!s->subscription)
        {
            s->subscription = add_subscription(s->aeron, s->channel, s->stream_id);
            if (!s->subscription)
            {
                return 0;
            }
        }
        if (aeron_subscription_image_count(s->subscription) > 0)
        {
            s->image = aeron_subscription_image_at_index(s->subscription, 0);
            s->live  = true;
            return 1;
        }
        return 0;
    }
}

static void gap_close(echo_state_t *base)
{
    gap_state_t *s = (gap_state_t *)base;
    if (s->subscription)
    {
        aeron_subscription_close(s->subscription, NULL, NULL);
        s->subscription = NULL;
    }
}

static gap_state_t *gap_state_create(
    aeron_t *aeron,
    echo_context_t *echo_ctx,
    const char *channel,
    int32_t stream_id,
    int64_t connection_timeout_ns)
{
    gap_state_t *s = calloc(1, sizeof(gap_state_t));
    if (!s)
    {
        return NULL;
    }

    s->base.await_connected  = gap_await_connected;
    s->base.poll             = gap_poll;
    s->base.close            = gap_close;
    s->aeron                 = aeron;
    s->echo_ctx              = echo_ctx;
    s->channel               = channel;
    s->stream_id             = stream_id;
    s->connection_timeout_ns = connection_timeout_ns;
    s->live                  = false;

    s->subscription = add_subscription(aeron, channel, stream_id);
    if (!s->subscription)
    {
        free(s);
        return NULL;
    }

    return s;
}

typedef struct
{
    int32_t session_id;
    int count;
} recording_session_holder_t;

static void recording_descriptor_consumer(
    aeron_archive_recording_descriptor_t *descriptor,
    void *clientd)
{
    recording_session_holder_t *h = (recording_session_holder_t *)clientd;
    h->session_id = descriptor->session_id;
    h->count++;
}

typedef struct
{
    echo_state_t base;

    aeron_t *aeron;
    echo_context_t *echo_ctx;
    aeron_archive_t *aeron_archive;

    aeron_subscription_t *live_subscription;
    aeron_image_t *image;
    bool live;

    aeron_subscription_t *merge_subscription;
    aeron_archive_replay_merge_t *replay_merge;
    int64_t lost_position;
    int recovery_attempts;
    int64_t recovery_start_ns;
    int64_t recovery_deadline_ns;
    int64_t recovery_archive_fragments;
    int64_t recovery_live_fragments;
    bool recovery_in_archive_phase;

    const char *live_channel;
    int32_t live_stream_id;
    const char *replay_channel;
    const char *replay_destination;
    int64_t recording_id;
    int64_t connection_timeout_ns;
} replay_merge_state_t;

static void replay_merge_await_connected(echo_state_t *base)
{
    (void)base;
}

static int replay_merge_do_live(replay_merge_state_t *s)
{
    if (aeron_image_is_closed(s->image))
    {
        s->lost_position = aeron_image_position(s->image);
        aeron_subscription_close(s->live_subscription, NULL, NULL);
        s->live_subscription = NULL;
        s->image             = NULL;
        s->live              = false;
        return 1;
    }
    return aeron_image_poll(s->image, on_fragment, s->echo_ctx, FRAGMENT_LIMIT_DEFAULT);
}

static int replay_merge_do_recovery(replay_merge_state_t *s)
{
    if (s->replay_merge && aeron_nano_clock() > s->recovery_deadline_ns)
    {
        fprintf(stderr,
            "ReplayMerge TIMED OUT on attempt #%d"
            ", lostPosition=%" PRId64
            ", elapsedMs=%" PRId64
            ", archiveFragments=%" PRId64
            ", liveFragments=%" PRId64 "\n",
            s->recovery_attempts,
            s->lost_position,
            (int64_t)((aeron_nano_clock() - s->recovery_start_ns) / 1000000LL),
            s->recovery_archive_fragments,
            s->recovery_live_fragments);
        exit(1);
    }

    if (!s->replay_merge)
    {
        int64_t archive_position;
        if (aeron_archive_get_recording_position(&archive_position, s->aeron_archive, s->recording_id) < 0)
        {
            fprintf(stderr, "aeron_archive_get_recording_position failed: %s\n", aeron_errmsg());
            return 0;
        }

        const int64_t start_position = (s->lost_position < 0) ? 0 : s->lost_position;

        if (archive_position <= start_position)
        {
            return 0;
        }

        s->recovery_attempts++;
        s->recovery_start_ns          = aeron_nano_clock();
        s->recovery_deadline_ns       = s->recovery_start_ns + s->connection_timeout_ns;
        s->recovery_archive_fragments = 0;
        s->recovery_live_fragments    = 0;
        s->recovery_in_archive_phase  = true;

        recording_session_holder_t holder = { 0, 0 };
        int32_t found_count = 0;
        if (aeron_archive_list_recording(
                &found_count,
                s->aeron_archive,
                s->recording_id,
                recording_descriptor_consumer,
                &holder) < 0 || found_count == 0)
        {
            fprintf(stderr, "aeron_archive_list_recording failed or not found: %s\n", aeron_errmsg());
            return 0;
        }

        const int32_t recording_session_id = holder.session_id;

        aeron_uri_string_builder_t rb;
        aeron_uri_string_builder_init_on_string(&rb, s->replay_channel);
        aeron_uri_string_builder_put_int32(&rb, AERON_URI_SESSION_ID_KEY, recording_session_id);
        char resolved_replay_channel[AERON_URI_MAX_LENGTH];
        aeron_uri_string_builder_sprint(&rb, resolved_replay_channel, sizeof(resolved_replay_channel));
        aeron_uri_string_builder_close(&rb);

        aeron_uri_string_builder_t lb;
        aeron_uri_string_builder_init_on_string(&lb, s->live_channel);
        const char *gtag = aeron_uri_string_builder_get(&lb, AERON_URI_GTAG_KEY);

        aeron_uri_string_builder_t mb;
        aeron_uri_string_builder_init_new(&mb);
        aeron_uri_string_builder_put(&mb, AERON_URI_STRING_BUILDER_MEDIA_KEY, "udp");
        aeron_uri_string_builder_put(&mb, AERON_UDP_CHANNEL_CONTROL_MODE_KEY,
            AERON_UDP_CHANNEL_CONTROL_MODE_MANUAL_VALUE);
        aeron_uri_string_builder_put_int32(&mb, AERON_URI_SESSION_ID_KEY, recording_session_id);
        if (gtag)
        {
            aeron_uri_string_builder_put(&mb, AERON_URI_GTAG_KEY, gtag);
        }

        char merge_channel[AERON_URI_MAX_LENGTH];
        aeron_uri_string_builder_sprint(&mb, merge_channel, sizeof(merge_channel));
        aeron_uri_string_builder_close(&mb);
        aeron_uri_string_builder_close(&lb);

        s->merge_subscription = add_subscription(s->aeron, merge_channel, s->live_stream_id);
        if (!s->merge_subscription)
        {
            fprintf(stderr, "Failed to create merge subscription\n");
            return 0;
        }

        if (aeron_archive_replay_merge_init(
                &s->replay_merge,
                s->merge_subscription,
                s->aeron_archive,
                resolved_replay_channel,
                s->replay_destination,
                s->live_channel,
                s->recording_id,
                start_position,
                (long long)aeron_epoch_clock(),
                REPLAY_MERGE_PROGRESS_TIMEOUT_DEFAULT_MS) < 0)
        {
            fprintf(stderr, "aeron_archive_replay_merge_init failed: %s\n", aeron_errmsg());
            aeron_subscription_close(s->merge_subscription, NULL, NULL);
            s->merge_subscription = NULL;
            return 0;
        }

        return 1;
    }

    if (aeron_archive_replay_merge_has_failed(s->replay_merge))
    {
        fprintf(stderr,
            "ReplayMerge FAILED on attempt #%d"
            ", lostPosition=%" PRId64
            ", archiveFragments=%" PRId64
            ", liveFragments=%" PRId64 "\n",
            s->recovery_attempts,
            s->lost_position,
            s->recovery_archive_fragments,
            s->recovery_live_fragments);
        exit(1);
    }

    const int fragments = aeron_archive_replay_merge_poll(
        s->replay_merge, on_fragment, s->echo_ctx, FRAGMENT_LIMIT_DEFAULT);

    if (fragments < 0)
    {
        fprintf(stderr, "aeron_archive_replay_merge_poll failed: %s\n", aeron_errmsg());
        exit(1);
    }

    if (s->recovery_in_archive_phase)
    {
        s->recovery_archive_fragments += fragments;
        if (aeron_archive_replay_merge_is_live_added(s->replay_merge))
        {
            s->recovery_in_archive_phase = false;
        }
    }
    else
    {
        s->recovery_live_fragments += fragments;
    }

    if (aeron_archive_replay_merge_is_merged(s->replay_merge))
    {
        s->image              = aeron_archive_replay_merge_image(s->replay_merge);
        s->live_subscription  = s->merge_subscription;
        s->merge_subscription = NULL;

        aeron_archive_replay_merge_close(s->replay_merge);
        s->replay_merge = NULL;

        s->live = true;
        return 1;
    }

    return fragments;
}

static int replay_merge_poll(echo_state_t *base)
{
    replay_merge_state_t *s = (replay_merge_state_t *)base;
    return s->live ? replay_merge_do_live(s) : replay_merge_do_recovery(s);
}

static void replay_merge_close(echo_state_t *base)
{
    replay_merge_state_t *s = (replay_merge_state_t *)base;

    if (s->replay_merge)
    {
        aeron_archive_replay_merge_close(s->replay_merge);
        s->replay_merge = NULL;
    }
    if (s->merge_subscription)
    {
        aeron_subscription_close(s->merge_subscription, NULL, NULL);
        s->merge_subscription = NULL;
    }
    if (s->live_subscription)
    {
        aeron_subscription_close(s->live_subscription, NULL, NULL);
        s->live_subscription = NULL;
    }
    if (s->aeron_archive)
    {
        aeron_archive_close(s->aeron_archive);
        s->aeron_archive = NULL;
    }
}

static replay_merge_state_t *replay_merge_state_create(
    aeron_t *aeron,
    echo_context_t *echo_ctx,
    const properties_t *props,
    const char *live_channel,
    int32_t live_stream_id,
    int64_t connection_timeout_ns)
{
    replay_merge_state_t *s = calloc(1, sizeof(replay_merge_state_t));
    if (!s)
    {
        return NULL;
    }

    s->base.await_connected  = replay_merge_await_connected;
    s->base.poll             = replay_merge_poll;
    s->base.close            = replay_merge_close;
    s->aeron                 = aeron;
    s->echo_ctx              = echo_ctx;
    s->live_channel          = live_channel;
    s->live_stream_id        = live_stream_id;
    s->connection_timeout_ns = connection_timeout_ns;
    s->lost_position         = -1;
    s->recovery_deadline_ns  = INT64_MAX;
    s->live                  = false;

    s->replay_channel    = properties_get(props, "recovering.echo.replay.channel", NULL);
    s->replay_destination = properties_get(props, "recovering.echo.replay.destination", NULL);
    s->recording_id      = properties_get_int64(props, "recovering.echo.recording.id", 0);

    if (!s->replay_channel || !s->replay_destination)
    {
        fprintf(stderr, "Missing recovering.echo.replay.channel or recovering.echo.replay.destination\n");
        free(s);
        return NULL;
    }

    const char *control_channel  = properties_get(props, "recovering.echo.archive.control.channel", NULL);
    const char *control_response = properties_get(props, "recovering.echo.archive.control.response.channel", NULL);
    int32_t control_stream       = properties_get_int32(props, "recovering.echo.archive.control.stream", 0);

    if (!control_channel || !control_response)
    {
        fprintf(stderr, "Missing archive control channel or response channel\n");
        free(s);
        return NULL;
    }

    printf("ReplayMergeState: connecting to archive"
           " controlChannel=%s, controlStream=%d, responseChannel=%s\n",
        control_channel, control_stream, control_response);

    aeron_archive_context_t *archive_ctx = NULL;
    if (aeron_archive_context_init(&archive_ctx) < 0)
    {
        fprintf(stderr, "aeron_archive_context_init failed: %s\n", aeron_errmsg());
        free(s);
        return NULL;
    }

    aeron_archive_context_set_aeron(archive_ctx, aeron);
    aeron_archive_context_set_owns_aeron_client(archive_ctx, false);
    aeron_archive_context_set_control_request_channel(archive_ctx, control_channel);
    aeron_archive_context_set_control_request_stream_id(archive_ctx, control_stream);
    aeron_archive_context_set_control_response_channel(archive_ctx, control_response);

    if (aeron_archive_connect(&s->aeron_archive, archive_ctx) < 0)
    {
        fprintf(stderr, "aeron_archive_connect failed: %s\n", aeron_errmsg());
        aeron_archive_context_close(archive_ctx);
        free(s);
        return NULL;
    }

    aeron_archive_context_close(archive_ctx);

    printf("  archive connected"
           ", recordingId=%" PRId64
           ", replayChannel=%s"
           ", replayDestination=%s"
           ", liveDestination=%s\n",
        s->recording_id, s->replay_channel, s->replay_destination, live_channel);

    return s;
}

typedef struct
{
    echo_state_t base;
    aeron_archive_persistent_subscription_t *persistent_subscription;
    aeron_archive_persistent_subscription_context_t *ps_ctx;
    aeron_archive_context_t *archive_ctx;
    echo_context_t *echo_ctx;
} persistent_subscription_state_t;

static void persistent_subscription_await_connected(echo_state_t *base)
{
    (void)base;
}

static int persistent_subscription_poll(echo_state_t *base)
{
    persistent_subscription_state_t *s = (persistent_subscription_state_t *)base;

    static int64_t poll_count = 0;
    if (++poll_count == 1 || poll_count % 10000000 == 0)
    {
        printf("persistent_subscription_poll: count=%" PRId64 "\n", poll_count);
        fflush(stdout);
    }

    return aeron_archive_persistent_subscription_controlled_poll(
        s->persistent_subscription,
        on_fragment_controlled,
        s->echo_ctx,
        FRAGMENT_LIMIT_DEFAULT);
}

static void persistent_subscription_close(echo_state_t *base)
{
    persistent_subscription_state_t *s = (persistent_subscription_state_t *)base;
    if (s->persistent_subscription)
    {
        aeron_archive_persistent_subscription_close(s->persistent_subscription);
        s->persistent_subscription = NULL;
    }
    if (s->ps_ctx)
    {
        aeron_archive_persistent_subscription_context_close(s->ps_ctx);
        s->ps_ctx = NULL;
    }
    if (s->archive_ctx)
    {
        aeron_archive_context_close(s->archive_ctx);
        s->archive_ctx = NULL;
    }
}

static persistent_subscription_state_t *persistent_subscription_state_create(
    aeron_t *aeron,
    echo_context_t *echo_ctx,
    const properties_t *props,
    const char *live_channel,
    int32_t live_stream_id)
{
    persistent_subscription_state_t *s = calloc(1, sizeof(persistent_subscription_state_t));
    if (!s)
    {
        return NULL;
    }

    s->base.await_connected = persistent_subscription_await_connected;
    s->base.poll            = persistent_subscription_poll;
    s->base.close           = persistent_subscription_close;
    s->echo_ctx             = echo_ctx;
    s->ps_ctx               = NULL;
    s->archive_ctx          = NULL;

    const char *control_channel  = properties_get(props, "recovering.echo.archive.control.channel", NULL);
    const char *control_response = properties_get(props, "recovering.echo.archive.control.response.channel", NULL);
    int32_t     control_stream   = properties_get_int32(props, "recovering.echo.archive.control.stream", 0);
    int64_t     recording_id     = properties_get_int64(props, "recovering.echo.recording.id", 0);
    const char *replay_channel   = properties_get(props, "recovering.echo.replay.channel", NULL);
    int32_t     replay_stream_id = properties_get_int32(props, "recovering.echo.replay.stream", -5);

    if (!control_channel || !control_response)
    {
        fprintf(stderr, "Missing archive control channel or response channel\n");
        free(s);
        return NULL;
    }

    if (!replay_channel)
    {
        fprintf(stderr, "Missing recovering.echo.replay.channel\n");
        free(s);
        return NULL;
    }

    printf("PersistentSubscriptionState: connecting to archive"
           " controlChannel=%s, controlStream=%d, responseChannel=%s\n",
        control_channel, control_stream, control_response);

    aeron_archive_context_t *archive_ctx = NULL;
    if (aeron_archive_context_init(&archive_ctx) < 0)
    {
        fprintf(stderr, "aeron_archive_context_init failed: %s\n", aeron_errmsg());
        free(s);
        return NULL;
    }

    aeron_archive_context_set_control_request_channel(archive_ctx, control_channel);
    aeron_archive_context_set_control_request_stream_id(archive_ctx, control_stream);
    aeron_archive_context_set_control_response_channel(archive_ctx, control_response);

    aeron_archive_persistent_subscription_context_t *ps_ctx = NULL;
    if (aeron_archive_persistent_subscription_context_init(&ps_ctx) < 0)
    {
        fprintf(stderr, "aeron_archive_persistent_subscription_context_init failed: %s\n", aeron_errmsg());
        aeron_archive_context_close(archive_ctx);
        free(s);
        return NULL;
    }

    aeron_archive_persistent_subscription_context_set_aeron(ps_ctx, aeron);
    aeron_archive_persistent_subscription_context_set_archive_context(ps_ctx, archive_ctx);
    aeron_archive_persistent_subscription_context_set_recording_id(ps_ctx, recording_id);
    aeron_archive_persistent_subscription_context_set_live_channel(ps_ctx, live_channel);
    aeron_archive_persistent_subscription_context_set_live_stream_id(ps_ctx, live_stream_id);
    aeron_archive_persistent_subscription_context_set_replay_channel(ps_ctx, replay_channel);
    aeron_archive_persistent_subscription_context_set_replay_stream_id(ps_ctx, replay_stream_id);
    aeron_archive_persistent_subscription_context_set_start_position(ps_ctx, AERON_PERSISTENT_SUBSCRIPTION_FROM_START);

    if (aeron_archive_persistent_subscription_create(&s->persistent_subscription, ps_ctx) < 0)
    {
        fprintf(stderr, "aeron_archive_persistent_subscription_create failed: %s\n", aeron_errmsg());
        aeron_archive_persistent_subscription_context_close(ps_ctx);
        aeron_archive_context_close(archive_ctx);
        free(s);
        return NULL;
    }

    s->ps_ctx      = ps_ctx;
    s->archive_ctx = archive_ctx;

    printf("  PersistentSubscription created."
           " recordingId=%" PRId64
           ", liveChannel=%s, liveStreamId=%d"
           ", replayChannel=%s, replayStreamId=%d\n",
        recording_id, live_channel, live_stream_id, replay_channel, replay_stream_id);

    return s;
}

static volatile bool g_running = true;

static void signal_handler(int signum)
{
    (void)signum;
    g_running = false;
}

int main(int argc, char **argv)
{
    if (argc < 2)
    {
        fprintf(stderr, "Usage: %s <benchmark.properties> [<override.properties> ...]\n", argv[0]);
        return 1;
    }

    properties_t props;
    memset(&props, 0, sizeof(props));

    for (int i = 1; i < argc; i++)
    {
        properties_t file_props;
        memset(&file_props, 0, sizeof(file_props));
        if (properties_load(&file_props, argv[i]) < 0)
        {
            return 1;
        }
        properties_merge(&props, &file_props);
    }

    const char *recovery_mode =
        properties_get(&props, "recovering.echo.recovery.mode", "GAP");
    const char *destination_channel =
        properties_get(&props, "io.aeron.benchmarks.aeron.destination.channel",
            "aeron:udp?endpoint=localhost:13333|mtu=1408");
    const char *source_channel =
        properties_get(&props, "io.aeron.benchmarks.aeron.source.channel",
            "aeron:udp?endpoint=localhost:13334|mtu=1408");
    int32_t destination_stream_id =
        properties_get_int32(&props, "io.aeron.benchmarks.aeron.destination.stream", 77777);
    int32_t source_stream_id =
        properties_get_int32(&props, "io.aeron.benchmarks.aeron.source.stream", 55555);
    int32_t receiver_index =
        properties_get_int32(&props, "io.aeron.benchmarks.aeron.receiver.index", 0);
    const int64_t connection_timeout_ns = CONNECTION_TIMEOUT_NS_DEFAULT;

    printf("RecoveringEchoNode.init(recoveryMode=%s, receiverIndex=%d)\n",
        recovery_mode, receiver_index);
    printf("  publication  (source):      channel=%s stream=%d\n",
        source_channel, source_stream_id);
    printf("  subscription (destination): channel=%s stream=%d\n",
        destination_channel, destination_stream_id);

    signal(SIGINT,  signal_handler);
    signal(SIGTERM, signal_handler);

    aeron_context_t *aeron_ctx = NULL;
    if (aeron_context_init(&aeron_ctx) < 0)
    {
        fprintf(stderr, "aeron_context_init failed: %s\n", aeron_errmsg());
        return 1;
    }

    const char *aeron_dir = properties_get(&props, "aeron.dir", NULL);
    if (aeron_dir)
    {
        aeron_context_set_dir(aeron_ctx, aeron_dir);
    }

    aeron_t *aeron = NULL;
    if (aeron_init(&aeron, aeron_ctx) < 0 || aeron_start(aeron) < 0)
    {
        fprintf(stderr, "aeron_init/start failed: %s\n", aeron_errmsg());
        return 1;
    }

    aeron_async_add_exclusive_publication_t *async_pub = NULL;
    if (aeron_async_add_exclusive_publication(&async_pub, aeron, source_channel, source_stream_id) < 0)
    {
        fprintf(stderr, "aeron_async_add_exclusive_publication failed: %s\n", aeron_errmsg());
        return 1;
    }

    aeron_exclusive_publication_t *publication = NULL;
    int pub_result;
    while ((pub_result = aeron_async_add_exclusive_publication_poll(&publication, async_pub)) == 0)
    {
        sched_yield();
    }

    if (pub_result < 0 || !publication)
    {
        fprintf(stderr, "Failed to create exclusive publication: %s\n", aeron_errmsg());
        return 1;
    }

    {
        aeron_publication_constants_t c;
        aeron_exclusive_publication_constants(publication, &c);
        printf("  publication created: sessionId=%d\n", c.session_id);
    }

    printf("  awaiting publication connected...\n");
    {
        const int64_t deadline_ns = aeron_nano_clock() + connection_timeout_ns;
        while (!aeron_exclusive_publication_is_connected(publication))
        {
            if (aeron_nano_clock() > deadline_ns)
            {
                fprintf(stderr, "Publication failed to connect within timeout\n");
                return 1;
            }
            sched_yield();
        }
    }
    printf("  publication connected\n");

    echo_context_t echo_ctx = {
        .publication      = publication,
        .receiver_index   = receiver_index,
        .running          = &g_running,
        .requests_received = 0,
        .responses_sent   = 0,
        .running_checksum = 0,
    };

    echo_state_t *echo_state = NULL;

    if (strcmp(recovery_mode, "GAP") == 0)
    {
        gap_state_t *gs = gap_state_create(
            aeron, &echo_ctx,
            destination_channel, destination_stream_id,
            connection_timeout_ns);
        if (!gs)
        {
            return 1;
        }
        echo_state = &gs->base;
    }
    else if (strcmp(recovery_mode, "REPLAY_MERGE") == 0)
    {
        replay_merge_state_t *rms = replay_merge_state_create(
            aeron, &echo_ctx, &props,
            destination_channel, destination_stream_id,
            connection_timeout_ns);
        if (!rms)
        {
            return 1;
        }
        echo_state = &rms->base;
    }
    else if (strcmp(recovery_mode, "PERSISTENT_SUBSCRIPTION") == 0)
    {
        persistent_subscription_state_t *pss = persistent_subscription_state_create(
            aeron, &echo_ctx, &props,
            destination_channel, destination_stream_id);
        if (!pss)
        {
            return 1;
        }
        echo_state = &pss->base;
    }
    else
    {
        fprintf(stderr, "Unknown recovery mode: %s\n", recovery_mode);
        return 1;
    }

    const char *idle_strategy_name =
        properties_get(&props, "io.aeron.benchmarks.aeron.idle.strategy", "spin");

    aeron_idle_strategy_func_t idle_func = NULL;
    void *idle_state = NULL;

    idle_func = aeron_idle_strategy_load(idle_strategy_name, &idle_state, NULL, NULL);
    if (!idle_func)
    {
        fprintf(stderr, "Failed to load idle strategy '%s': %s\n", idle_strategy_name, aeron_errmsg());
        return 1;
    }
    printf("  idle strategy: %s\n", idle_strategy_name);

    printf("  state created. Waiting for connection...\n");
    echo_state->await_connected(echo_state);
    printf("RecoveringEchoNode ready\n");

    const int64_t loop_start_ns = aeron_nano_clock();
    int64_t iterations = 0;

    while (g_running)
    {
        const int work = echo_state->poll(echo_state);
        idle_func(idle_state, work);
        iterations++;
    }

    const int64_t loop_end_ns = aeron_nano_clock();
    printf("Loop ran for %.3f ms, iterations=%" PRId64 "\n",
        (double)(loop_end_ns - loop_start_ns) / 1000000.0, iterations);

    printf("Requests received: %" PRId64 "\n", echo_ctx.requests_received);
    printf("Responses sent:    %" PRId64 "\n", echo_ctx.responses_sent);
    printf("Running checksum:  0x%016" PRIX64 "\n", echo_ctx.running_checksum);

    echo_state->close(echo_state);
    free(echo_state);

    aeron_exclusive_publication_close(publication, NULL, NULL);
    aeron_close(aeron);
    aeron_context_close(aeron_ctx);

    printf("RecoveringEchoNode stopped\n");
    return 0;
}