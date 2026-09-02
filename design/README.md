# catfight game design

What catfight is trying to become, and the decisions made about it.

Like `netplay/README.md` and `mapping/README.md`, this lives next to the thing
it describes and records reasoning rather than just conclusions, so the *why*
survives longer than the conversation it came from.

Scope: this document is the **game**. `netplay/README.md` is the
infrastructure and remains the source of truth for anything about servers,
matchmaking, identity or Steam. Where they overlap, netplay wins on mechanism
and this document wins on intent.

Status: **the shooter exists and works. Everything else here is intent.**
Movement, rounds, elimination, teams and damage are built and tested;
companions, the economy, cosmetics and the house are not.

---

## The game

You and an AI companion live in a house together. You fight alongside her,
and you spend what you win on cosmetics — for her, for yourself, and
eventually for the house.

**Matches are 2v2: one player and one AI companion per team.** Matchmaking
pairs two *humans*; each brings their own companion, so a match is 2 players
and 2 bots. The companion is not an escort mission and not decoration — her
health and ammo sit on the player's HUD, she may carry different stats, and the
player commands her during the fight.

This supersedes the 2026-08-22 framing of "two teams of one". What does not
change is the reason that framing was chosen: **all rules are written in terms
of teams, never in terms of "the other player"**, which is exactly what makes
adding a second body per side additive rather than a rewrite.

### Where player power comes from — now settled

The 2026-08-22 note recorded this as undecided between map pickups, fixed
loadout, and hybrid. It is now **cosmetics**: currency is earned in matches and
spent on appearance.

Nothing yet says cosmetics carry gameplay effect, and the default assumption is
that they do not — a competitive game that sells power is a different and much
harder product. **Confirm before building items either way.**

---

## The differentiator

Not the companion. Not the anthro art. **Natural-language tactical command of a
bot teammate inside a competitive shooter.**

Almost every LLM in games today writes flavour text. This is a *mechanic*: what
the player says changes how the round plays. It is also precisely the thing
large studios avoid, for reasons that are entirely legitimate — it is
unpredictable, hard to QA, and a competitive-integrity risk. That is the bet.

It is also what makes the companion structurally necessary rather than
decorative, which matters more than it sounds: a competitive multiplayer game
from an unknown developer dies of **cold start**. Empty queues, no matches,
refunds. A game where you and your companion can fight, and progression
continues, **at population zero** survives a launch week with forty players.
The companion is the answer to the problem that kills indie multiplayer.

### Architecture: the LLM never controls the bot

```
voice/text  →  intent parse  →  one of N discrete commands  →  deterministic behaviour
```

The bot executes a small, hand-tuned, fully testable vocabulary — hold, push,
fall back, focus that one, follow me, go left, wait here. The LLM is only a
natural-language front end onto that vocabulary.

This is not a compromise, it is what makes the feature shippable:

- **Latency.** Even a fast local model costs hundreds of milliseconds.
  Commands must therefore be *tactical and positional*, never twitch — you
  issue intent, you do not aim her. Common phrasings can be cached to nothing.
- **Determinism.** If one phrase produces different behaviour run to run,
  competitive players revolt. Non-negotiable. A fixed command set is testable
  in a way a free-running model is not.
- **Degradation.** Model slow, absent, or confused falls back to the menu and
  the game still works.
- **Swappability.** A new model only has to map words onto the same enum.

### Build order, matching this project's habits

`netplay/README.md` already establishes *console commands before UI* — build
the mechanism, then present it. Same rule here:

1. **The discrete command set first, on a radial menu or keybinds. No LLM.**
2. Tune the bot behaviours until commanding her is fun with a menu.
3. *Then* add natural language as a layer over something that already works.

If step 1 is not fun, an LLM will not save it — and that is knowable in weeks
rather than after building an inference pipeline.

### Models

Local, and hot-swappable by design: whatever the best available model is at the
time. Three rules that make that cheap:

- Everything behind a narrow interface — prompt in, filtered text out.
- No model-specific formatting ever reaches game code.
- **The guardrail layer lives outside the model**, so it survives every swap.
  A new model means new jailbreaks; guardrails that live in the prompt do not
  carry over, and guardrails that live in the pipeline do.

Local models also sidestep the largest real risk in this space. See below.

---

## Scope and ordering

Early access, ship-and-expand. The reference is **Hades, Valheim,
Satisfactory** — not No Man's Sky, which is the cautionary version of the same
model: it launched without a working core loop and spent years recovering.
Early access forgives missing content and punishes a hollow centre.

**The centre is the fighting. It has to be good at launch.**

1. **Shooter feel, solo-testable.** Where the project is now.
2. **The commandable combat bot.** Near-term and load-bearing — the
   differentiator, and the cold-start answer.
3. **Matchmaking and the economy**, so that winning means something.
4. **Home as a menu only** — talk, ready up, buy cosmetics. Deliberately
   delayed for a long time.
5. **The house as a place.**
6. **LLM personality.** Last. The local-model landscape will have moved by then
   anyway, which suits the swap-friendly design.

### On bot quality

Both teams have a companion, which makes bot strength a **fairness** problem
rather than a quality one — symmetric mediocrity is balanced. The bar is
therefore not *skill*, it is **consistency**: erratic bots make matches feel
decided by RNG rather than by play, and that is the version competitive players
will not forgive.

### A verified constraint for when companions land

Bots are not free slots. `SV_BotAllocateClient` (`code/server/sv_bot.c:47-60`)
walks the same `svs.clients` array humans use, bounded by `sv_maxclients`, and
returns `-1` when no slot is `CS_FREE`. **`sv_maxclients` means clients
including bots.**

It stays at **2** while no bots exist. The day companions arrive it must become
**4**, and the two-human cap moves into `cfmm` and team assignment where it
belongs. Left at 2 with bots present, both companions silently fail to spawn
and the game module only sees a `-1`.

---

## Characters and content

Characters have anthropomorphic options but **always a human personality and
human bone structure**. This is a hard art-direction rule, not a preference,
and it is doing real work: human skeletal proportions are what keep anthro
characters classified as stylized people rather than animals, which is the
distinction that matters to storefronts and payment processors alike.

### The proportion clamp — the highest-leverage decision in the design

**Adult proportions are enforced in the character creator in code, not in art
direction.** Clamp height, head-to-body ratio and limb proportions so that no
combination of sliders produces a childlike silhouette. Test the *corners* of
the parameter space, not the defaults — age reads through proportion far more
than through any single measurement, and a large head on a small body reads
young regardless of everything else.

The reason this matters more than it first appears: the creator does not merely
avoid depicting minors. **It caps the severity of every future LLM failure.** A
jailbroken model saying something crude to an obviously adult character is an
embarrassing clip. The identical output attached to a childlike avatar is
existential — a permanent ban with no appeal, and in most jurisdictions a
criminal matter rather than a terms-of-service one.

The clamp converts an unbounded risk into a bounded one, and it costs nothing
but discipline in a system that has not been built yet. Build it in from the
first version of the creator.

### Steam content policy, as researched 2026-08-26

Steam separates **sexy** from **sexual**. The Adult Only filter is for explicit
nudity, depicted sex acts, or content whose primary purpose is sexual
gratification. Revealing outfits, flirtation, innuendo and romance are all
mainstream — Nikke and Stellar Blade sit unfiltered in the open store, and
Baldur's Gate 3 has on-screen sex and still only carries a Mature Content
Description.

**The concept as designed lands in mature-descriptor territory, not Adult
Only.** Plan for the mainstream store.

The strategic point is that AO is a *business* decision more than a content
one: behind the filter you lose store discovery, most streamers, a lot of
organic traffic, and some regions entirely. So the rule is not "stay safe", it
is **do not sit near the line** — either be clearly on the mainstream side, or
commit to AO deliberately because the audience justifies it. The middle is
where a policy shift lands on you, and one did in 2025.

That 2025 shift is worth knowing precisely: payment processors, not Valve,
moved the boundary, and they targeted **themes** — non-consent, incest — rather
than explicitness, including in text-only content. The modern risk surface is
*what a thing is about*, not how much skin is visible.

**Caveat: Steam's adult rules are the fastest-moving part of its policy, and
this was researched against training data current to May 2026. Re-verify before
anything depends on it.**

### AI disclosure and jailbreaks

Steam has required **AI content disclosure** since January 2024, with a
stricter written requirement for *live-generated* content: you must describe
what guardrails prevent the model producing illegal content. That is an answer
Valve evaluates, not a checkbox. Drafting it early forces the design, so it is
worth writing before it is needed.

On the fear that one viral jailbreak clip ends the project — it does not.
**Valve bans products, not incidents.** The question asked is what a product is
*for* and what its developer did when something went wrong, and an isolated
jailbreak that was fixed and can be documented is survivable. The realistic
worst case is an email and a patch.

The instructive precedent is **AI Dungeon (2021)**, and its lesson is not the
one people remember: Latitude was not removed from any storefront. Their *model
provider* threatened to cut them off, which forced an emergency filtering
rollout, which caused a second scandal about false positives. The damage came
from upstream. **Local models sidestep that failure mode entirely**, which is
an argument for the choice already made.

What makes an incident survivable, concretely:

- **Filter output separately from the model.** Model alignment is the first
  layer, never the only one.
- **Make the filter avatar-aware** — the question is whether this dialogue is
  sexual *given this character's current appearance*, which needs both inputs.
- **Log and timestamp guardrail changes.** "Detected Tuesday, patched Thursday"
  is the entire defence.
- **Ship an in-game report button.** It turns players into detection and
  demonstrates good faith.

And a calibration for testing: the failure rate matters less than whether bad
output is reachable *by accident*. Nobody blames a developer for what a user
extracted after forty-five minutes of determined jailbreaking — that reads as
the user's project. What does damage is bad output surfacing unprompted to
someone who was not trying. Test toward that.

---

## Honest risks

Recorded because they are easy to forget and expensive to rediscover.

**Scope.** The full concept is three or four games — a competitive shooter with
original netcode, a life sim, an LLM companion, a character creator, a
decoration system, an economy, and competent combat AI. Early access makes that
tractable *only* if the centre ships good and narrow. The ordering above is the
mitigation; treat it as load-bearing rather than aspirational.

**Free-form domestic content.** "Do whatever you want in the house" has
unbounded surface area and no natural completion state, which is where sandbox
projects die. Hence the house being a menu for a long time, and a place only
once there is a reason to be in it.

**Author distance.** The developer is not a romance-game consumer, though they
have said they understand the appeal and are "a fan of minor anthro stuff".
That is a normal and workable position — taste for the thousand small calls is
there. The residual risk is not craft, it is **motivation across a multi-year
build** by someone who has said they would probably not play the finished game.
Worth knowing about; not worth redesigning around.
