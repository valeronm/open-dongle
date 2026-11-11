# Enclosure

This folder holds the case as a [SolidCraft](https://github.com/tangency-labs/solidcraft) model,
`case.sc`, and as `tray.stl` and `lid.stl` ready to print. It needs no screws, inserts or glue: the
board drops into the tray, and the lid snaps on with its ribs holding the board down along the
bare strips at its long edges.

Print the tray floor down and the lid face down. The walls are sized for a 0.4 mm nozzle.

If a test print doesn't fit, adjust these parameters in `case.sc` and export the STLs again:

- `fit` sets the clearance on each side, between the board and the walls and between the lid and
  the tray.
- `clamp` sets how far the lid's ribs reach down onto the board. Raise it if the board rattles.
- `snap_h` sets the height of the lid's snap bumps. Raise it for a firmer click, or lower it if
  the lid is hard to close or remove.
