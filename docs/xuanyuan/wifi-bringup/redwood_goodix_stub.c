/*
 * Stand-in for redwood_goodix.c's entry point when the Goodix/FocalTech driver is left out of the
 * build (it includes redwood_focaltech_fw.h, an untracked generated header in the original tree).
 * The real one only starts anything behind boot-arg touchscreen=1, which this port never sets.
 */
void
redwood_goodix_init(void)
{
}
