/* MCUboot loads initialized RTC data through its instruction alias.
 * The allocator initializes this bank after boot, through the data alias. */
.fc_rtc_heap (NOLOAD) :
{
    *(.fc_heap.rtc_fast)
} GROUP_LINK_IN(rtc_data_seg)
