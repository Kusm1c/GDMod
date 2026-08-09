// Decompiled from FUN_14039a1d0 (win 0x39a1d0), C:\Users\Kusmic\decompGD\GeometryDash.exe.c
// lines 452622-452696. Field names per this session's confirmed PlayerObject offset
// map (m_isUpsideDown=0x9bf, m_yVelocity=0x9a0, m_isBall=0x9bb, m_isDart=0x9bc).
// Several called functions (FUN_1403a0680, FUN_140397760, FUN_1403a0220,
// FUN_140398860, FUN_1403a0090, FUN_14038d480, FUN_14039add0) are NOT yet
// identified/traced — left as raw FUN_ names; almost certainly animation/
// particle-effect triggers (state-change color flash, dictionary/cache resets),
// not physics. Several field names below are GUESSED from context, not confirmed
// against the real header — each is annotated with its raw offset so it can be
// re-checked. This is reference/pseudocode only, not meant to compile.
void PlayerObject::flipGravity(bool newUpsideDown, bool silentFlag /* param_3 */) {
    if (m_isUpsideDown == newUpsideDown) return;

    FUN_1403a0680(); // unidentified — called unconditionally on any flip

    m_isUpsideDown = newUpsideDown;

    if (m_gameLayer) {
        // gameEventTriggered-style dispatch: 10 = flipped to upside-down, 11 = flipped
        // back to right-side-up (matches the 0xa/0xb codes seen elsewhere this session
        // via FUN_140231ff0, the generic "notify state change" dispatcher).
        gameEventTriggered(newUpsideDown ? 10 : 11, 0);
    }

    // Reset some per-flip bookkeeping — exact semantics of these specific fields
    // not traced; offsets given rather than guessed names.
    field_0x800 /* = field_0xaa0, a position/time snapshot */ = field_0xaa0;
    field_0x960 = 0;
    field_0x958 = 0;
    field_0xa29_bool = false;

    // If either of two unidentified bools (0x9b0/0x9b1 — possibly dual-mode/reverse
    // related) is set, toggle a "going left"-ish bool at 0x68c.
    if (field_0x9b1_bool || field_0x9b0_bool) {
        m_isGoingLeft = !m_isGoingLeft; // offset 0x68c — name guessed from context
    }

    // Clear four cached dictionaries/collections (touched-object logs?) and three
    // "last X" sentinels back to -1 — likely per-object collision/trigger caches
    // that are orientation-dependent and must be invalidated on a flip.
    dict_0x5b0->removeAllObjects();
    dict_0x5b8->removeAllObjects();
    dict_0x5c0->removeAllObjects();
    dict_0x5c8->removeAllObjects();
    field_0x5e0 = -1;
    field_0x5d0 = -1;
    field_0x5d8 = -1;

    // PHYSICS: halve the current y-velocity on a flip — but only if a flag at
    // offset 0x7e1 (name unknown, called `field_0x7e1_bool` here) is false. Matches
    // gdsim's existing halving on vehicle-transition (Vehicle.cpp's ship()/cube()
    // enter() lambdas) — this is the same real mechanic, confirmed here to also
    // apply to a plain gravity-portal flip (not just a vehicle-mode change).
    if (!field_0x7e1_bool) {
        m_yVelocity *= 0.5;
        if (!silentFlag) {
            // Colored flash effect (green-ish tint if flipping to upside-down: RGB
            // triplet 0x9600/0xff0000-ish; pink/red if flipping back: 0xc8ff/...).
            // Purely visual, not physics.
            FUN_140397760(this, /* color bytes, elided */ nullptr, 45.0f /* 0x42340000 */);
        }
    }

    FUN_1403a0220(this); // unidentified
    FUN_140398860(this); // unidentified

    if (!m_isBall) {
        // One-time GameManager-backed check (some global toggle, cached in a static
        // GameManager-adjacent struct DAT_1406c2ed8) gating a dash-ring/particle
        // effect (FUN_1403a0090) when not wave(dart) and the 0x7e1 flag above is
        // false. Not physics.
        static void* s_cachedManagerLikeThing = nullptr; // DAT_1406c2ed8
        bool cachedManagerFlagSet = false; // *(cachedThing+0x41)... != 0 && ...==0
        if (!s_cachedManagerLikeThing) { /* lazy-init via FUN_1404d0770/FUN_14017ab00 */ }
        if (cachedManagerFlagSet && !m_isDart && !field_0x7e1_bool) {
            field_0x7e3_bool = true;
            FUN_1403a0090(this);
        }
    }

    field_0xa2c /* name guessed */ = field_0xa00 /* name guessed */;
    m_isOnGround = false;

    if (m_isBall) {
        m_rotationSpeed = 0;   // was 2-byte field at 0x728
        field_0x668_bool = false;
        field_0x720_int  = 0;
        FUN_14038d480(this);   // unidentified — likely ball-rotation reset
        return;
    }
    if (m_isDart) {
        FUN_14039add0(this);  // unidentified — likely wave-trail reset
    }
}
