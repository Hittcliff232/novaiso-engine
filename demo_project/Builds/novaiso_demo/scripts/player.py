MOVE_SPEED = 260.0
JUMP_SPEED = -680.0


def on_spawn(entity, scene):
    scene.play_music("assets/audio/03_-southampton.mp3", True)
    scene.log("Player spawned. Use A/D to move, Space to jump, Tab to swap camera.")


def on_update(entity, scene, dt):
    vx = 0.0
    if scene.action_down("MoveLeft"):
        vx -= MOVE_SPEED
    if scene.action_down("MoveRight"):
        vx += MOVE_SPEED

    vy = entity.velocity()[1]
    if scene.action_down("Jump") and entity.grounded():
        vy = JUMP_SPEED
        scene.play_sound("assets/jump.wav")

    entity.set_velocity(vx, vy)


def on_trigger(entity, scene, trigger_name):
    scene.log(f"Entered trigger: {trigger_name}")

