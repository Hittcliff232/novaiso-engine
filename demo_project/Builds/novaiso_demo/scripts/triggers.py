import json


def always_true(scene, trigger_name, args_json):
    return True


def require_side(scene, trigger_name, args_json):
    return scene.camera_mode() == "side"


def toggle_camera(scene, trigger_name, args_json):
    scene.toggle_camera_mode()
    scene.log(f"Camera swapped by {trigger_name}")


def log_message(scene, trigger_name, args_json):
    data = json.loads(args_json or "{}")
    scene.log(data.get("message", f"Trigger fired: {trigger_name}"))


def play_sound(scene, trigger_name, args_json):
    data = json.loads(args_json or "{}")
    scene.play_sound(data.get("sound", "assets/jump.wav"))
