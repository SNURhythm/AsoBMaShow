package com.snurhythm.asobmashow;

import java.util.Calendar;
import java.util.GregorianCalendar;
import java.util.TimeZone;

/** Gregorian civil dates shared with the native Kombo model, in the current zone. */
final class KomboReminderTime {
    static long at(int year, int month, int day, int hour, int minute, TimeZone zone) {
        Calendar date = new GregorianCalendar(zone);
        date.clear();
        date.set(year, month - 1, day, hour, minute);
        return date.getTimeInMillis();
    }
    static boolean isDue(long now, int year, int month, int day, int hour, int minute, TimeZone zone) {
        Calendar date = new GregorianCalendar(zone);
        date.setTimeInMillis(now);
        return date.get(Calendar.YEAR) == year && date.get(Calendar.MONTH) + 1 == month &&
                date.get(Calendar.DAY_OF_MONTH) == day && at(year, month, day, hour, minute, zone) <= now;
    }
    private KomboReminderTime() {}
}
