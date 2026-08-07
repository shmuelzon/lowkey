show_bottom = true;
show_top = true;
show_buttons = true;
$fn = 180;

EPSILON = 0.01;
MECHANICAL_CLEARANCE = 0.1;

WALL_THICKNESS = 1.2;

CASE_DIAMETER = 34;
CASE_HEIGHT = 5.5;
DOME_HEIGHT = 4;

SCREW_LOCATIONS = [
  [8.25, 7.25],
  [-7.75, 7.25],
];

BUTTONS_LOCATIONS = [
  [7, -0.15],
  [-6.75, -0.25],
];

module m2_countersunk_screw(length)
{
  color("gray")
  {
    cylinder(h=1.4, r1=1.8 + MECHANICAL_CLEARANCE, r2=1 + MECHANICAL_CLEARANCE);
    translate([0, 0, 1.4])cylinder(h=length, r=1 + MECHANICAL_CLEARANCE);
  }
}

module m2_insert()
{
  INSERT_DIAMETER=3.45;
  INSERT_HEIGHT=5;

  color("red")
  {
    cylinder(h=0.5, r1=(INSERT_DIAMETER / 2) + 0.2, r2=(INSERT_DIAMETER * 0.9 ) / 2);
    translate([0, 0, 0.5]) cylinder(h=INSERT_HEIGHT - 0.5, r=(INSERT_DIAMETER * 0.9 ) / 2);
  }

}

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

module dome(r, h)
{
  difference()
  {
    intersection()
    {
      scale([1, 1, h / r]) sphere(r = r);
      translate([-r, -r, 0]) cube([r * 2, r * 2, h]);
    } 
    intersection()
    {
      scale([1, 1, (h - WALL_THICKNESS) / (r - WALL_THICKNESS)]) sphere(r=r - WALL_THICKNESS);
      translate([-r, -r, -0.01]) cube([r * 2, r * 2, h]);
    }
  }
}

module pcb() {
  translate([0, 0, 4.28 + WALL_THICKNESS])
  color("green", 1)
  rotate([0, 0, 180])
  import("ui_pcb.stl", center=true);
}

module case_bottom() {
  difference()
  {
    union()
    {
      cylinder(r=CASE_DIAMETER / 2 - MECHANICAL_CLEARANCE, h=WALL_THICKNESS);
      intersection()
      {
        translate([-5, -20, 0]) cube([10, 7, WALL_THICKNESS]);
        cylinder(r=CASE_DIAMETER / 2 + WALL_THICKNESS, h=WALL_THICKNESS);
      }
    }
    // FFC
    translate([-5 - MECHANICAL_CLEARANCE, -19, 0]) rotate([10, 0, 0])  cube([10 + MECHANICAL_CLEARANCE * 2, 7, 2]);
  }
}

module case_top() {
  difference()
  {
    cylinder(r=CASE_DIAMETER / 2 + WALL_THICKNESS, h=CASE_HEIGHT);
    cylinder(r=CASE_DIAMETER / 2, h=CASE_HEIGHT + EPSILON);
    // FFC
    translate([-5 - MECHANICAL_CLEARANCE, -20, 0]) cube([10 + MECHANICAL_CLEARANCE * 2, 7, 1]);
  }
  translate([0, 0, CASE_HEIGHT]) dome(r=CASE_DIAMETER / 2 + WALL_THICKNESS, h=DOME_HEIGHT);
  // Insert pillars
  for (screw_location = SCREW_LOCATIONS)
    translate([screw_location[0], screw_location[1], 3]) cylinder(h=5, r=3);
  // stand-offs
  translate([9, -7.5, 3]) cylinder(h=5, r=1.5);
  translate([-9, -7.5, 3]) cylinder(h=5, r=1.5);

  translate([BUTTONS_LOCATIONS[0][0], -12, CASE_HEIGHT + DOME_HEIGHT - 1]) linear_extrude(1.2) import("lock.svg", center=true);
  translate([BUTTONS_LOCATIONS[1][0], -12, CASE_HEIGHT + DOME_HEIGHT - 1]) linear_extrude(1.2) import("unlock.svg", center=true);
}

module buttons() {
  for (button_location = BUTTONS_LOCATIONS)
  {
    translate([button_location[0], button_location[1], 5.6])
    difference()
    {
      union()
      {
        cylinder(h=4.9, r=2 - MECHANICAL_CLEARANCE);
        cylinder(h=1, r=3);
      }
      cylinder(h=4.2, r=1.5);
    }
  }
}

module cutouts() {
  color("pink", 0.5)
  {
    // USB-C
    translate([0.08, 8.35, WALL_THICKNESS]) round_cube([9, 3.5, 10], 1.2);
    // Buttons
    for (button_location = BUTTONS_LOCATIONS)
      translate([button_location[0], button_location[1], 7]) cylinder(h=3, r=2);
  }
}

module hardware()
{
  for (screw_location = SCREW_LOCATIONS)
  {
    translate([screw_location[0], screw_location[1], 0]) m2_countersunk_screw(5.5);
    translate([screw_location[0], screw_location[1], 3]) m2_insert();
  }
}

difference()
{
  union()
  {
    if (show_top) case_top();
    if (show_bottom) case_bottom();
  }
  pcb();
  cutouts();
  hardware();
}

difference()
{
  if (show_buttons) buttons();
  //pcb();
}

if ($preview)
{
  pcb();
  cutouts();
  hardware();
}

