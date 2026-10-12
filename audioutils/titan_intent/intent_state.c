/****************************************************************************
 * apps/audioutils/titan_intent/intent_state.c
 *
 * SPDX-License-Identifier: Apache-2.0
 ****************************************************************************/

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <string.h>
#include <audioutils/titan_intent.h>

/****************************************************************************
 * Private Functions
 ****************************************************************************/

static void titan_event(struct titan_intent_s *ctx, uint64_t now,
                        unsigned int kind,
                        struct titan_intent_event_s *event)
{
  memset(event, 0, sizeof(*event));
  event->version = TITAN_INTENT_VERSION;
  event->seq = ++ctx->state.seq;
  event->utterance_id = ctx->current;
  event->monotonic_ms = now;
  event->generation = ctx->state.generation;
  event->kind = kind;
  event->intent = TITAN_UNKNOWN;
  event->resolved = TITAN_MODE_NONE;
  memcpy(event->active, ctx->state.active, sizeof(event->active));
}

static int titan_mode(unsigned int intent)
{
  switch (intent)
    {
      case TITAN_VIDEO_START:
      case TITAN_VIDEO_STOP:
        return TITAN_MODE_VIDEO;

      case TITAN_AUDIO_START:
      case TITAN_AUDIO_STOP:
        return TITAN_MODE_AUDIO;

      case TITAN_COMPANION_ENTER:
      case TITAN_COMPANION_EXIT:
        return TITAN_MODE_COMPANION;

      default:
        return TITAN_MODE_NONE;
    }
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

void titan_intent_init(struct titan_intent_s *ctx,
                       const struct titan_intent_policy_s *policy,
                       uint32_t generation)
{
  memset(ctx, 0, sizeof(*ctx));
  ctx->policy = *policy;
  ctx->state.generation = generation;
}

bool titan_intent_begin(struct titan_intent_s *ctx, uint64_t utterance,
                        uint64_t now, uint32_t generation)
{
  if (generation != ctx->state.generation || utterance == 0 ||
      utterance <= ctx->last_turn || ctx->in_turn)
    {
      return false;
    }

  ctx->current = utterance;
  ctx->in_turn = true;
  ctx->emitted = false;
  ctx->inline_seen = false;
  ctx->window_seen = false;
  ctx->invalid = false;
  ctx->wake_history = 0;
  ctx->window_history = 0;
  ctx->authorized = ctx->state.active[TITAN_MODE_COMPANION] ||
                    (ctx->state.wake_deadline != 0 &&
                     now <= ctx->state.wake_deadline);

  /* The next turn consumes the window regardless of its classification.
   * Authorization remains attached to this turn if it ends after expiry.
   */

  ctx->state.wake_deadline = 0;
  return true;
}

bool titan_intent_candidate(struct titan_intent_s *ctx, uint64_t utterance,
                            uint32_t generation, uint64_t now,
                            const struct titan_intent_score_s *score,
                            bool too_long,
                            struct titan_intent_event_s *event)
{
  unsigned int intent = score->label;
  int target;
  int i;
  bool start;
  bool changed;

  if (!ctx->in_turn || ctx->current != utterance || ctx->emitted ||
      ctx->invalid || generation != ctx->state.generation)
    {
      return false;
    }

  if (too_long)
    {
      ctx->invalid = true;
      ctx->state.too_long++;
      return false;
    }

  if (ctx->window_seen)
    {
      return false;
    }

  if (!titan_intent_score_accept(score, TITAN_INTENT_ACTIONS,
                                 ctx->policy.command_score,
                                 ctx->policy.command_margin))
    {
      ctx->state.unknown++;
      return false;
    }

  if (!ctx->authorized && !ctx->inline_seen)
    {
      ctx->state.denied++;
      return false;
    }

  ctx->emitted = true;
  titan_event(ctx, now, TITAN_EVENT_INTENT, event);
  event->intent = intent;
  event->score = score->score;
  event->margin = score->margin;
  if ((ctx->action_valid & (1u << intent)) != 0 &&
      now - ctx->last_action[intent] < ctx->policy.cooldown_ms)
    {
      event->result = TITAN_RESULT_COOLDOWN;
      ctx->state.cooldown++;
      return true;
    }

  ctx->action_valid |= 1u << intent;
  ctx->last_action[intent] = now;
  target = titan_mode(intent);
  start = intent == TITAN_VIDEO_START || intent == TITAN_AUDIO_START ||
          intent == TITAN_COMPANION_ENTER;
  if (intent == TITAN_STOP_RECENT)
    {
      for (i = 0; i < TITAN_INTENT_MODES; i++)
        {
          if (ctx->state.active[i] &&
              (target < 0 ||
               ctx->state.started[i] > ctx->state.started[target]))
            {
              target = i;
            }
        }
    }

  changed = intent == TITAN_PHOTO_TAKE;
  if (target >= 0)
    {
      changed = ctx->state.active[target] != start;
      ctx->state.active[target] = start;
      if (start && changed)
        {
          ctx->state.started[target] = ++ctx->state.order;
        }

      if (target == TITAN_MODE_COMPANION && !start)
        {
          ctx->state.wake_deadline = 0;
          ctx->inline_seen = false;
          ctx->window_seen = false;
        }
    }

  event->resolved = target;
  event->result = changed ? TITAN_RESULT_APPLIED : TITAN_RESULT_NOOP;
  memcpy(event->active, ctx->state.active, sizeof(event->active));
  return true;
}

bool titan_intent_end(struct titan_intent_s *ctx, uint64_t utterance,
                      uint64_t now, struct titan_intent_event_s *event)
{
  bool wake;

  if (!ctx->in_turn || ctx->current != utterance)
    {
      return false;
    }

  wake = !ctx->invalid && !ctx->emitted &&
         (ctx->window_seen || ctx->inline_seen);
  if (wake)
    {
      titan_event(ctx, now, ctx->window_seen ? TITAN_EVENT_WAKE_WINDOW :
                  TITAN_EVENT_WAKE_INLINE, event);
      event->score = ctx->wake.score;
      event->margin = ctx->wake.margin;
      if (ctx->window_seen)
        {
          ctx->state.wake_deadline = now + ctx->policy.window_ms;
        }
    }

  ctx->last_turn = utterance;
  ctx->in_turn = false;
  return wake;
}

void titan_intent_cancel(struct titan_intent_s *ctx)
{
  if (ctx->in_turn)
    {
      ctx->last_turn = ctx->current;
    }

  ctx->in_turn = false;
  ctx->inline_seen = false;
  ctx->window_seen = false;
  ctx->wake_history = 0;
  ctx->window_history = 0;
}

void titan_intent_reset(struct titan_intent_s *ctx, uint64_t now,
                        struct titan_intent_event_s *event)
{
  struct titan_intent_policy_s policy = ctx->policy;
  uint32_t generation = ctx->state.generation + 1;
  uint64_t seq = ctx->state.seq;

  titan_intent_init(ctx, &policy, generation);
  ctx->state.seq = seq;
  titan_event(ctx, now, TITAN_EVENT_RESET, event);
}

const char *titan_intent_name(unsigned int intent)
{
  static const char * const names[TITAN_INTENT_CLASSES] =
  {
    "photo_take", "video_start", "video_stop", "audio_start",
    "audio_stop", "companion_enter", "companion_exit", "stop_recent",
    "unknown", "silence"
  };

  return intent < TITAN_INTENT_CLASSES ? names[intent] : "unknown";
}

const char *titan_intent_event_name(unsigned int kind)
{
  static const char * const names[] =
  {
    "intent", "wake_inline", "wake_window", "reset"
  };

  return kind <= TITAN_EVENT_RESET ? names[kind] : "unknown";
}

const char *titan_intent_result_name(unsigned int result)
{
  static const char * const names[] =
  {
    "applied", "noop", "cooldown"
  };

  return result <= TITAN_RESULT_COOLDOWN ? names[result] : "unknown";
}
