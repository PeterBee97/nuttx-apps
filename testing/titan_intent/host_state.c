/****************************************************************************
 * apps/testing/titan_intent/host_state.c
 *
 * SPDX-License-Identifier: Apache-2.0
 ****************************************************************************/

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <assert.h>
#include <stdio.h>
#include <audioutils/titan_intent.h>

/****************************************************************************
 * Private Data
 ****************************************************************************/

static uint64_t g_turn;
static uint64_t g_now = 1000;

/****************************************************************************
 * Private Functions
 ****************************************************************************/

static void wake(struct titan_intent_s *ctx, unsigned int label)
{
  struct titan_intent_score_s score =
  {
    30000, 20000, label
  };

  titan_intent_wake(ctx, &score);
  titan_intent_wake(ctx, &score);
}

static bool command(struct titan_intent_s *ctx, unsigned int label,
                    bool inline_wake, struct titan_intent_event_s *event)
{
  struct titan_intent_score_s score =
  {
    30000, 20000, label
  };

  struct titan_intent_event_s ended;
  bool result;

  g_now += 1100;
  assert(titan_intent_begin(ctx, ++g_turn, g_now, ctx->state.generation));
  if (inline_wake)
    {
      wake(ctx, TITAN_WAKE_INLINE);
    }

  result = titan_intent_candidate(ctx, g_turn, ctx->state.generation,
                                  g_now + 250, &score, false, event);
  assert(!titan_intent_candidate(ctx, g_turn, ctx->state.generation,
                                 g_now + 260, &score, false, &ended) ||
         !result);
  titan_intent_end(ctx, g_turn, g_now + 800, &ended);
  return result;
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

int main(void)
{
  struct titan_intent_policy_s policy =
  {
    8000, 1000, 20000, 3000, 20000, 3000
  };

  struct titan_intent_score_s score =
  {
    30000, 20000, TITAN_PHOTO_TAKE
  };

  struct titan_intent_event_s event;
  struct titan_intent_s ctx;
  int8_t logits[10] =
  {
    0
  };

  uint32_t generation;

  titan_intent_init(&ctx, &policy, 1);
  assert(!command(&ctx, TITAN_PHOTO_TAKE, false, &event));
  assert(command(&ctx, TITAN_PHOTO_TAKE, true, &event));
  assert(event.kind == TITAN_EVENT_INTENT);
  assert(event.intent == TITAN_PHOTO_TAKE);
  assert(event.result == TITAN_RESULT_APPLIED);

  /* A single wake word never authorizes the next utterance. */

  assert(titan_intent_begin(&ctx, ++g_turn, g_now, 1));
  wake(&ctx, TITAN_WAKE_INLINE);
  assert(titan_intent_end(&ctx, g_turn, g_now, &event));
  assert(event.kind == TITAN_EVENT_WAKE_INLINE);
  assert(ctx.state.wake_deadline == 0);
  assert(!command(&ctx, TITAN_VIDEO_START, false, &event));

  /* A double wake supersedes an inline candidate, even with a command. */

  assert(titan_intent_begin(&ctx, ++g_turn, g_now, 1));
  wake(&ctx, TITAN_WAKE_INLINE);
  wake(&ctx, TITAN_WAKE_WINDOW);
  assert(!titan_intent_candidate(&ctx, g_turn, 1, g_now, &score, false,
                                 &event));
  assert(titan_intent_end(&ctx, g_turn, g_now, &event));
  assert(event.kind == TITAN_EVENT_WAKE_WINDOW);
  assert(ctx.state.wake_deadline == g_now + 8000);
  assert(command(&ctx, TITAN_VIDEO_START, false, &event));
  assert(ctx.state.active[TITAN_MODE_VIDEO]);
  assert(ctx.state.wake_deadline == 0);
  assert(!command(&ctx, TITAN_PHOTO_TAKE, false, &event));

  /* Authorization is locked at utterance start, not classification time. */

  ctx.state.wake_deadline = g_now + 100;
  assert(titan_intent_begin(&ctx, ++g_turn, g_now + 99, 1));
  assert(titan_intent_candidate(&ctx, g_turn, 1, g_now + 2000, &score,
                                false, &event));
  titan_intent_end(&ctx, g_turn, g_now + 2500, &event);
  ctx.state.wake_deadline = g_now - 1;
  assert(!command(&ctx, TITAN_PHOTO_TAKE, false, &event));

  assert(command(&ctx, TITAN_VIDEO_STOP, true, &event));
  assert(command(&ctx, TITAN_AUDIO_START, true, &event));
  assert(command(&ctx, TITAN_VIDEO_START, true, &event));
  assert(command(&ctx, TITAN_AUDIO_START, true, &event));
  assert(event.result == TITAN_RESULT_NOOP);
  assert(command(&ctx, TITAN_STOP_RECENT, true, &event));
  assert(event.resolved == TITAN_MODE_VIDEO);
  assert(ctx.state.active[TITAN_MODE_AUDIO]);
  assert(!ctx.state.active[TITAN_MODE_VIDEO]);
  assert(command(&ctx, TITAN_STOP_RECENT, true, &event));
  assert(event.resolved == TITAN_MODE_AUDIO);
  assert(command(&ctx, TITAN_STOP_RECENT, true, &event));
  assert(event.result == TITAN_RESULT_NOOP);

  assert(command(&ctx, TITAN_COMPANION_ENTER, true, &event));
  assert(command(&ctx, TITAN_AUDIO_START, false, &event));
  assert(command(&ctx, TITAN_COMPANION_EXIT, false, &event));
  assert(ctx.state.active[TITAN_MODE_AUDIO]);
  assert(!command(&ctx, TITAN_PHOTO_TAKE, false, &event));

  /* Unknown consumes an authorized window. Failure and stale generations
   * must not change any mode or execute a later result from the same turn.
   */

  ctx.state.wake_deadline = g_now + 8000;
  assert(!command(&ctx, TITAN_UNKNOWN, false, &event));
  assert(!command(&ctx, TITAN_VIDEO_START, false, &event));
  assert(titan_intent_begin(&ctx, ++g_turn, g_now, 1));
  wake(&ctx, TITAN_WAKE_INLINE);
  assert(!titan_intent_candidate(&ctx, g_turn, 1, g_now, &score, true,
                                 &event));
  assert(!titan_intent_end(&ctx, g_turn, g_now, &event));
  assert(ctx.state.too_long == 1);
  assert(titan_intent_begin(&ctx, ++g_turn, g_now, 1));
  wake(&ctx, TITAN_WAKE_INLINE);
  titan_intent_cancel(&ctx);
  assert(!titan_intent_candidate(&ctx, g_turn, 1, g_now, &score, false,
                                 &event));
  generation = ctx.state.generation;
  titan_intent_reset(&ctx, g_now, &event);
  assert(event.kind == TITAN_EVENT_RESET);
  assert(ctx.state.generation != generation);
  assert(!ctx.state.active[TITAN_MODE_AUDIO]);
  assert(!titan_intent_begin(&ctx, ++g_turn, g_now, generation));

  /* Cooldown suppresses only the same action, across utterances. */

  assert(command(&ctx, TITAN_PHOTO_TAKE, true, &event));
  assert(titan_intent_begin(&ctx, ++g_turn, g_now + 300,
                            ctx.state.generation));
  wake(&ctx, TITAN_WAKE_INLINE);
  assert(titan_intent_candidate(&ctx, g_turn, ctx.state.generation,
                                g_now + 350, &score, false, &event));
  assert(event.result == TITAN_RESULT_COOLDOWN);
  titan_intent_end(&ctx, g_turn, g_now + 800, &event);
  assert(command(&ctx, TITAN_VIDEO_START, true, &event));
  assert(command(&ctx, TITAN_VIDEO_STOP, true, &event));

  titan_intent_score(logits, 10, 1048576, &score);
  assert(score.score >= 3276 && score.score <= 3278);
  assert(score.margin == 0);
  logits[3] = 10;
  titan_intent_score(logits, 10, 1048576, &score);
  assert(score.label == 3 && score.score > 32700);
  assert(score.margin > 32700);
  puts("PASS authorization, wake precedence, expiry, recent-mode, dedup,"
       " cooldown, reset, discontinuity and score");
  return 0;
}
