// Plant Buddy - Front Shell
// Nico-style rounded shell with two leaf ears, OLED window, and button hole.

$fn = 48;

// Core dimensions (mm)
wall = 2.0;
tol = 0.35;
corner_r = 5;

oled_module_w = 26.0;   // 2.6 cm OLED module width
oled_module_h = 30.0;   // 3.0 cm OLED module height
display_total_h = oled_module_h;
oled_glass_w = 25.0;    // 2.5 cm window width
oled_glass_h = 17.0;    // 1.7 cm window height

inner_w = 48.0;
inner_h = 45.0;
front_total_depth = 35.0; // 3.5 cm front-only depth (front to back)
inner_d = front_total_depth - wall;

ext_w = inner_w + wall * 2;
ext_h = inner_h + wall * 2;
ext_d = inner_d + wall;

usb_w = 10.5;
usb_h = 4.2;

button_r = 2.2;
eps = 0.2;

module rounded_box(w, h, d, r) {
    hull() {
        for (x = [r, w - r])
            for (y = [r, h - r])
                translate([x, y, 0])
                    cylinder(r = r, h = d);
    }
}

module leaf_ear(height, base_w, tip_shift, depth) {
    translate([0, 0, -depth / 2])
    linear_extrude(height = depth) {
        // Leaf silhouette kept solid to avoid split volumes.
        hull() {
            scale([1.0, 0.50]) circle(r = base_w / 2);
            translate([tip_shift * 0.45, height * 0.55])
                scale([0.75, 0.35]) circle(r = base_w / 2);
            translate([tip_shift, height])
                circle(r = 2.2);
        }
    }
}

difference() {
    union() {
        rounded_box(ext_w, ext_h, ext_d, corner_r);
    }

    // Main internal cavity.
    translate([wall, wall, wall - eps])
        rounded_box(inner_w, inner_h, ext_d + eps * 2, corner_r - 1.2);

    // OLED viewing window.
    oled_win_w = oled_glass_w;
    oled_win_h = oled_glass_h + 1.2;
    win_x = (ext_w - oled_win_w) / 2;
    win_y = (ext_h - display_total_h) / 2 + (display_total_h - oled_win_h) / 2;
    translate([win_x, win_y, -0.1])
        cube([oled_win_w, oled_win_h, wall + 0.2]);

    // USB-C opening (bottom edge).
    translate([ext_w / 2 - usb_w / 2, -0.1, wall + 1.8])
        cube([usb_w, wall + 0.2, usb_h]);

    // Right-side near-back wire hole (away from OLED side), support-free profile.
    right_back_hole_w = 21.0;
    right_back_hole_h = 6.0;
    right_back_hole_cy = ext_h - 12.0;
    right_back_hole_cz = ext_d - (wall + 4.2);
    translate([ext_w - 0.1, 0, 0])
        linear_extrude(height = wall + 0.2)
            polygon(points = [
                [right_back_hole_cy - right_back_hole_w / 2, right_back_hole_cz],
                [right_back_hole_cy, right_back_hole_cz + right_back_hole_h / 2],
                [right_back_hole_cy + right_back_hole_w / 2, right_back_hole_cz],
                [right_back_hole_cy, right_back_hole_cz - right_back_hole_h / 2]
            ]);

    // Large rectangular hole at marked front area (upper center of visible side).
    front_marked_hole_w = 22.0;
    front_marked_hole_h = 9.0;
    front_marked_hole_x = (ext_w - front_marked_hole_w) / 2;
    front_marked_hole_z = ext_d - 15.0;
    translate([front_marked_hole_x, -0.1, front_marked_hole_z])
        cube([front_marked_hole_w, wall + 0.2, front_marked_hole_h]);

    // Manual button hole near lower-right front.
    translate([ext_w - 10, 10, -0.1])
        cylinder(r = button_r, h = wall + 0.2);

}
