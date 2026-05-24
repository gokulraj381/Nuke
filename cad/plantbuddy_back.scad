// Plant Buddy - Back Shell
// Nico-style rounded dome back with snap lip, vent slots, and cable exit.

$fn = 48;

wall = 2.0;
corner_r = 5;
lip_h = 2.2;
lip_inset = 0.30;
lip_shell_overlap_z = -0.4;
lip_shell_overlap_xy = 0.55;
eps = 0.2;

inner_w = 48.0;
inner_h = 45.0;

ext_w = inner_w + wall * 2;
ext_h = inner_h + wall * 2;

dome_h = 10.0;

usb_w = 10.5;
usb_h = 4.2;

module rounded_box(w, h, d, r) {
    hull() {
        for (x = [r, w - r])
            for (y = [r, h - r])
                translate([x, y, 0])
                    cylinder(r = r, h = d);
    }
}

module dome_back(w, h, rise) {
    hull() {
        rounded_box(w, h, 0.15, corner_r);
        translate([4, 4, rise])
            rounded_box(w - 8, h - 8, 0.15, corner_r - 1.4);
    }
}

difference() {
    // Outer skin.
    dome_back(ext_w, ext_h, dome_h);

    // Hollow interior shell.
    translate([wall, wall, -eps])
        dome_back(ext_w - wall * 2, ext_h - wall * 2, dome_h - wall + eps);

    // USB opening alignment.
    translate([ext_w / 2 - usb_w / 2, -0.1, -0.1])
        cube([usb_w, wall + 0.2, usb_h + 1.0]);

    // Ventilation slots near top.
    for (i = [0:4])
        translate([ext_w / 2 - 10 + i * 5, ext_h - wall - 0.1, dome_h - 2])
            rotate([-90, 0, 0])
                cylinder(r = 1.0, h = wall + 0.2);
}

// Snap-fit lip that goes inside the front shell.
translate([wall + lip_inset - lip_shell_overlap_xy, wall + lip_inset - lip_shell_overlap_xy, lip_shell_overlap_z])
difference() {
    rounded_box(inner_w - lip_inset * 2, inner_h - lip_inset * 2, lip_h, corner_r - 1.5);
    translate([wall, wall, -0.1])
        rounded_box(inner_w - lip_inset * 2 - wall * 2, inner_h - lip_inset * 2 - wall * 2, lip_h + 0.2, corner_r - 2.3);
}

// Bridge tabs that tie the lip to shell rim (prevents multi-volume STL export).
translate([wall - 0.2, wall + 8, 0]) cube([0.9, 8, 1.8]);
translate([wall - 0.2, ext_h - wall - 16, 0]) cube([0.9, 8, 1.8]);
translate([ext_w - wall - 0.7, wall + 8, 0]) cube([0.9, 8, 1.8]);
translate([ext_w - wall - 0.7, ext_h - wall - 16, 0]) cube([0.9, 8, 1.8]);
