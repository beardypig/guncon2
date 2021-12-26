#!/usr/bin/env python3
import argparse
import re
import sys
import time
from collections import namedtuple
from math import floor, ceil
from queue import Queue

import pygame
import pygame.font
import evdev
from evdev import ecodes

import logging

log = logging.getLogger("guncon2-calibration")

Postion = namedtuple("Postion", ["x", "y"])


class Guncon2(object):
    def __init__(self, device):
        self.device = device
        self.pos = Postion(0, 0)

    @property
    def absinfo(self):
        return [self.device.absinfo(ecodes.ABS_X), self.device.absinfo(ecodes.ABS_Y)]

    @property
    def min_x(self):
        return self.device.absinfo(ecodes.ABS_X).min

    @property
    def max_x(self):
        return self.device.absinfo(ecodes.ABS_X).max

    @property
    def min_y(self):
        return self.device.absinfo(ecodes.ABS_Y).min

    @property
    def max_y(self):
        return self.device.absinfo(ecodes.ABS_Y).max

    @property
    def pos_normalised(self):
        return Postion(self.normalise(self.pos.x, self.min_x, self.max_x),
                       self.normalise(self.pos.y, self.min_y, self.max_y))

    @staticmethod
    def normalise(pos, min_, max_):
        return (pos - min_) / float(max_ - min_)

    def update(self):
        while True:
            ev = self.device.read_one()
            if ev:
                if ev.type == ecodes.EV_ABS:
                    if ev.code == ecodes.ABS_X:
                        self.pos = Postion(ev.value, self.pos.y)
                    elif ev.code == ecodes.ABS_Y:
                        self.pos = Postion(self.pos.x, ev.value)
                if ev.type == ecodes.EV_KEY:
                    yield ev.code, ev.value
            else:
                break

    def calibrate(self, targets, shots, width=320, height=240):
        targets_x = [target[0] for target in targets]
        targets_y = [target[1] for target in targets]
        shots_x = [shot[0] for shot in shots]
        shots_y = [shot[1] for shot in shots]

        # calculate the ratio between on-screen units and gun units for each axes
        try:
            gsratio_x = (max(targets_x) - min(targets_x)) / (max(shots_x) - min(shots_x))
        except ZeroDivisionError:
            log.error("Failed to calibrate X axis")
            return
        try:
            gsratio_y = (max(targets_y) - min(targets_y)) / (max(shots_y) - min(shots_y))
        except ZeroDivisionError:
            log.error("Failed to calibrate X axis")
            return

        min_x = min(shots_x) - (min(targets_x) * gsratio_x)
        max_x = max(shots_x) + ((width - max(targets_x)) * gsratio_x)

        min_y = min(shots_y) - (min(targets_y) * gsratio_y)
        max_y = max(shots_y) + ((height - max(targets_y)) * gsratio_y)

        # set the X and Y calibration values
        self.device.set_absinfo(ecodes.ABS_X, min=int(min_x), max=int(max_x))
        self.device.set_absinfo(ecodes.ABS_Y, min=int(min_y), max=int(max_y))

        log.info(f"Calibration: x=({self.absinfo[0]}) y=({self.absinfo[1]})")


WIDTH = 320
HEIGHT = 240
TARGET_SIZE = 20
WHITE = (255, 255, 255)
GREY = (128, 128, 128)

STATE_START = 0
STATE_TARGET = 1
STATE_DONE = 3


def draw_target(size=10):
    image = pygame.Surface((size * 8, size * 8)).convert()
    mid = (size * 8) // 2
    pygame.draw.circle(image, WHITE, (mid, mid), size * 4, 2)

    pygame.draw.line(image, WHITE, (mid, mid - size), (mid, mid + size), 2)
    pygame.draw.line(image, WHITE, (mid - size, mid), (mid + size, mid), 2)

    image.set_colorkey([0, 0, 0])
    return image


def draw_cursor(size=10, color=WHITE):
    image = pygame.Surface((size + 2, size + 2)).convert()
    mid = hsize = size // 2
    pygame.draw.line(image, color, (mid - hsize, mid - hsize), (mid - 2, mid - 2), 2)
    pygame.draw.line(image, color, (mid + hsize, mid - hsize), (mid + 2, mid - 2), 2)
    pygame.draw.line(image, color, (mid - hsize, mid + hsize), (mid - 2, mid + 2), 2)
    pygame.draw.line(image, color, (mid + hsize, mid + hsize), (mid + 2, mid + 2), 2)

    image.set_colorkey([0, 0, 0])
    return image


def blit_center(screen, image, pos):
    screen.blit(image, (pos[0] - (image.get_rect()[2] // 2), pos[1] - (image.get_rect()[3] // 2)), )


def blit_right(screen, image, pos):
    screen.blit(image, (pos[0] - (image.get_rect()[2]), pos[1]))


def main():
    def point_type(value):
        m = re.match(r"\(?(\d+)\s*,\s*(\d+)\)?", value)
        if m:
            return int(m.group(1)), int(m.group(2))
        else:
            raise ValueError("{} is an invalid point".format(value))

    parser = argparse.ArgumentParser()
    parser.add_argument("-r", "--resolution", default="320x240")
    parser.add_argument("--center-target", default=(160, 120), type=point_type)
    parser.add_argument("--topleft-target", default=(50, 50), type=point_type)
    parser.add_argument("--capture", default=None)
    args = parser.parse_args()

    logging.basicConfig(level=logging.INFO)

    try:
        w, h = args.resolution.split("x")
        width, height = int(w), int(h)
    except:
        parser.error("Invalid resolution, eg. 320x240")
        return

    guncon2_dev = None
    # find the first guncon2
    for device in [evdev.InputDevice(path) for path in evdev.list_devices()]:
        if device.name == "Namco GunCon 2":
            guncon2_dev = device
            break

    if guncon2_dev is None:
        sys.stderr.write("Failed to find any attached GunCon2 devices")
        return 1

    with guncon2_dev.grab_context():

        guncon = Guncon2(guncon2_dev)

        pygame.init()
        pygame.font.init()
        font = pygame.font.Font(None, 20)

        start_text = font.render("Pull the TRIGGER to start calibration", True, WHITE)
        start_text_w = start_text.get_rect()[2] // 2

        pygame.display.set_caption("GunCon 2 two-point calibration")

        screen = pygame.display.set_mode((width, height), pygame.FULLSCREEN)
        clock = pygame.time.Clock()

        state = STATE_START
        running = True
        targets = [(50, 50), (320 - 50, 50), (320 - 50, 240 - 50), (50, 240 - 50)]
        target_shots = [(0, 0), (0, 0), (0, 0), (0, 0)]

        cursor = draw_cursor(color=(255, 255, 0))
        target = draw_target()
        onscreen_warning = 0

        while running:
            for event in pygame.event.get():
                if event.type == pygame.QUIT or (event.type == pygame.KEYDOWN and event.key == pygame.K_q):
                    running = False

            screen.fill((80, 80, 80))

            raw_x, raw_y = guncon.pos
            cx, cy = int(guncon.pos_normalised.x * width), int(guncon.pos_normalised.y * height)
            trigger = False
            for button, value in guncon.update():
                if button == ecodes.BTN_LEFT and value == 1:
                    trigger = True

            raw_pos_txt = font.render(f"({raw_x}, {raw_y})", True, (128, 128, 255))
            cal_pos_txt = font.render(f"({cx}, {cy})", True, (128, 128, 255))

            screen.blit(raw_pos_txt, (20, height - 40))
            blit_right(screen, cal_pos_txt, (width - 20, height - 40))

            if state == STATE_START:
                screen.blit(start_text, ((width // 2) - start_text_w, height - 60))
                if width > cx >= 0 and height > cy >= 0:  # on screen
                    screen.blit(cursor, (cx, cy))
                if trigger:
                    state = STATE_TARGET
                    target_i = 0
                    log.info("Set target at: ({}, {})".format(*targets[target_i]))

            elif state == STATE_TARGET:
                blit_center(screen, target, targets[target_i])
                if raw_x > 5 and trigger:
                    target_shots[target_i] = (raw_x, raw_y)
                    target_i += 1
                    if target_i == len(targets):
                        state = STATE_DONE
                    else:
                        log.info("Set target at: ({}, {})".format(*targets[target_i]))

            elif state == STATE_DONE:
                guncon.calibrate(targets, target_shots)
                state = STATE_START

            # only trigger off screen shot on target states
            if raw_x < 5 and trigger and state != STATE_START:
                onscreen_warning = time.time() + 1.0

            if raw_x > 5 and trigger:
                onscreen_warning = 0

            if time.time() < onscreen_warning:
                off_screen_txt = font.render("Warning: Shot Off-Screen", True, (255, 80, 80))
                blit_center(screen, off_screen_txt, (width // 2, 60))

            fps = font.render(str(round(clock.get_fps())), True, (128, 128, 255))
            screen.blit(fps, (20, 20))

            pygame.display.flip()
            clock.tick(30)


if __name__ == "__main__":
    sys.exit(main() or 0)
