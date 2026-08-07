show_bottom = true;
show_top = true;
$fn = 36;

EPSILON = 0.01;
MECHANICAL_CLEARANCE = 0.1;

WALL_THICKNESS = 2;

CASE_WIDTH = 42;
CASE_DEPTH = 42;
CASE_HEIGHT = 13.8;

COVER_SIZE = [CASE_WIDTH - (WALL_THICKNESS * 2) - MECHANICAL_CLEARANCE, CASE_DEPTH - (WALL_THICKNESS * 2) - MECHANICAL_CLEARANCE, WALL_THICKNESS];

LOCK_OPENING = [27.2, WALL_THICKNESS];
BATTERY_OPENING = [WALL_THICKNESS, 6];
UI_OPENING = [13.5, WALL_THICKNESS];
PCB_AND_THROUGH_HOLE_HEIGHT = 3.6;

SCREW_LOCATIONS = [
  [6.3, 3.32],
  [-16.65, 2.97],
  [-1.95, -16.28],
];

module centered_cube(size=[0, 0, 0])
{
  translate([-size[0] / 2, -size[1] / 2, 0]) cube(size);
}

module round_cube(size, r)
{
  minkowski()
  { 
    centered_cube([size[0] - (2 * r), size[1] - (2 * r), size[2] - 0.01]);
    cylinder(r=r, h=0.01);
  }
}

module round_shell(size, wall_thickness1, r, wall_thickness2 = 0, r_inner = 0)
{
  difference()
  {
    round_cube(size, r);
    translate([0, 0, -EPSILON]) round_cube([size[0] - (wall_thickness1 * 2), size[1] - ((wall_thickness2 != 0 ? wall_thickness2 : wall_thickness1) * 2), size[2] + (2 * EPSILON)], (r_inner != 0 ? r_inner : r));
  }
}

module m2_countersunk_screw(length)
{
  color("gray")
  {
    cylinder(h=length, r=1 + MECHANICAL_CLEARANCE);
    translate([0, 0, length]) cylinder(h=1.4, r1=1 + MECHANICAL_CLEARANCE, r2=1.8 + MECHANICAL_CLEARANCE);
  }
}

module m2_insert()
{
  INSERT_DIAMETER=3.45;
  INSERT_HEIGHT=3;

  color("gold");
  {
    cylinder(h=INSERT_HEIGHT - 0.5, r=(INSERT_DIAMETER * 0.9 ) / 2);
    translate([0, 0, INSERT_HEIGHT - 0.5]) cylinder(h=0.5, r1=(INSERT_DIAMETER * 0.9 ) / 2, r2=(INSERT_DIAMETER / 2) + 0.2);
  }

}

module pcb() {
  translate([0, 0, 7]) color("gray", 0.5) import("controller_pcb.stl", center=true);
}

module case_bottom()
{
  difference()
  {
    round_cube([CASE_WIDTH, CASE_DEPTH, CASE_HEIGHT], 2);
    translate([0, 0, WALL_THICKNESS]) round_cube([CASE_WIDTH - (WALL_THICKNESS * 2), CASE_DEPTH - (WALL_THICKNESS * 2), CASE_HEIGHT - WALL_THICKNESS + EPSILON], 2);
    cutouts();
  }

  // Screw inserts housing
  for (screw_location = SCREW_LOCATIONS)
    translate([screw_location[0], screw_location[1], WALL_THICKNESS]) cylinder(r=4, h=2);
}

module case_top()
{
  translate([0, 0, CASE_HEIGHT - WALL_THICKNESS]) round_cube(COVER_SIZE, 2);

  // cutout covers
  translate([0, 0, CASE_HEIGHT - WALL_THICKNESS])
  {
    translate([-COVER_SIZE[0] / 2, COVER_SIZE[1] / 2 - (WALL_THICKNESS * 2) + 0.05, 0]) cube([LOCK_OPENING[0] + WALL_THICKNESS * 2, LOCK_OPENING[1] + WALL_THICKNESS * 2, WALL_THICKNESS]);
    translate([-COVER_SIZE[0]  / 2 - WALL_THICKNESS - 0.05, -COVER_SIZE[1] / 2, 0]) cube([BATTERY_OPENING[0] + (WALL_THICKNESS * 2), BATTERY_OPENING[1] + WALL_THICKNESS * 2, WALL_THICKNESS]);
    translate([(COVER_SIZE[0] / 2) - UI_OPENING[0] - WALL_THICKNESS * 2, -(COVER_SIZE[1] / 2 - UI_OPENING[1]) - WALL_THICKNESS * 2 - 0.05, 0]) cube([UI_OPENING[0] + WALL_THICKNESS * 2, UI_OPENING[1] + (WALL_THICKNESS * 2), WALL_THICKNESS]);
  }
}

module cutouts()
{
  translate([0, 0, WALL_THICKNESS + PCB_AND_THROUGH_HOLE_HEIGHT])
  {
    translate([-COVER_SIZE[0] / 2 - 0.05, COVER_SIZE[1] / 2 - WALL_THICKNESS + 0.05, 0]) cube([LOCK_OPENING[0] + WALL_THICKNESS * 2 + 0.05 * 2, LOCK_OPENING[1] + WALL_THICKNESS, CASE_HEIGHT - PCB_AND_THROUGH_HOLE_HEIGHT - WALL_THICKNESS]);
    translate([-COVER_SIZE[0]  / 2 - WALL_THICKNESS - 0.05, -COVER_SIZE[1] / 2 - 0.05, 0]) cube([BATTERY_OPENING[0] + WALL_THICKNESS, BATTERY_OPENING[1] + WALL_THICKNESS * 2 + 0.05 * 2, CASE_HEIGHT - PCB_AND_THROUGH_HOLE_HEIGHT - WALL_THICKNESS]);
    translate([(COVER_SIZE[0] / 2) - UI_OPENING[0] - WALL_THICKNESS * 2 - 0.05, -(COVER_SIZE[1] / 2 - UI_OPENING[1]) - WALL_THICKNESS * 2 - 0.05, 0]) cube([UI_OPENING[0] + WALL_THICKNESS * 2 + 0.05 * 2, UI_OPENING[1] + WALL_THICKNESS, CASE_HEIGHT - PCB_AND_THROUGH_HOLE_HEIGHT - WALL_THICKNESS]);
  }
}

module hardware()
{
  SCREW_HEIGHT = 10.8;
  for (screw_location = SCREW_LOCATIONS)
  {
    translate([screw_location[0], screw_location[1], WALL_THICKNESS - 0.4]) m2_countersunk_screw(SCREW_HEIGHT);
    translate([screw_location[0], screw_location[1], 1]) m2_insert();
  }
}

difference()
{
  union()
  {
    if (show_bottom) case_bottom();
    if (show_top) case_top();
  }
  pcb();
  hardware();
}

if ($preview)
{
  pcb();
  hardware();
}
