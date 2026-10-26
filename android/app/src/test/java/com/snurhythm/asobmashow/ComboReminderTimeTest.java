package com.snurhythm.asobmashow;

import static org.junit.Assert.*;
import org.junit.Test;
import java.util.TimeZone;
import java.util.Locale;

public final class ComboReminderTimeTest {
    @Test public void calendarDatesSurviveDaylightSavingChanges() {
        TimeZone zone = TimeZone.getTimeZone("America/New_York");
        long before = ComboReminderTime.at(2026, 3, 7, 20, 0, zone);
        long after = ComboReminderTime.at(2026, 3, 8, 20, 0, zone);
        assertEquals(23 * 3600000L, after - before);
        before = ComboReminderTime.at(2026, 10, 31, 20, 0, zone);
        after = ComboReminderTime.at(2026, 11, 1, 20, 0, zone);
        assertEquals(25 * 3600000L, after - before);
    }
    @Test public void delayedDeliveryCannotCatchUpOnAnotherDay() {
        TimeZone zone = TimeZone.getTimeZone("Asia/Seoul");
        long at = ComboReminderTime.at(2026, 12, 31, 20, 0, zone);
        assertFalse(ComboReminderTime.isDue(at - 1, 2026, 12, 31, 20, 0, zone));
        assertTrue(ComboReminderTime.isDue(at + 3600000, 2026, 12, 31, 20, 0, zone));
        assertFalse(ComboReminderTime.isDue(at + 4 * 3600000, 2026, 12, 31, 20, 0, zone));
    }
    @Test public void timezoneChangesRebaseTheSameLocalDate() {
        long seoul = ComboReminderTime.at(2026, 10, 27, 20, 0, TimeZone.getTimeZone("Asia/Seoul"));
        long london = ComboReminderTime.at(2026, 10, 27, 20, 0, TimeZone.getTimeZone("Europe/London"));
        assertEquals(9 * 3600000L, london - seoul);
    }
    @Test public void deviceLocaleDoesNotChangeGregorianYears() {
        Locale previous = Locale.getDefault();
        try {
            TimeZone utc = TimeZone.getTimeZone("UTC");
            long expected = ComboReminderTime.at(2026, 10, 27, 20, 0, utc);
            Locale.setDefault(new Locale("th", "TH"));
            assertEquals(expected, ComboReminderTime.at(2026, 10, 27, 20, 0, utc));
            assertTrue(ComboReminderTime.isDue(expected, 2026, 10, 27, 20, 0, utc));
        } finally { Locale.setDefault(previous); }
    }
}
