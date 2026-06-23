* Test frame renaming in Stata (with frame change)
clear all
version 16.0

sysuse auto, clear
local cur = c(frame)
di "Current frame is: `cur' (obs = " _N ")"

frame create stage
frame stage: set obs 5
frame stage: gen x = _n

capture noisily {
    * Rename current frame to a temp name
    frame rename `cur' old_default
    * Rename stage to the original current frame name
    frame rename stage `cur'
    * Change active frame to the new current frame
    frame change `cur'
    * Drop the old frame
    frame drop old_default
}
local rc = _rc
di "Frame rename rc = `rc'"
di "Current active frame after swap is: " c(frame) " (obs = " _N ")"
frame list
