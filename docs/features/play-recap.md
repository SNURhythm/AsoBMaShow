# Play recap

Open **Play recap** in the standard selector's chart-details tools or the skinned selector's **More** menu.

Choose **Session**, **Daily**, **Weekly**, or **Custom**. Previous/Next selects older/newer periods; Latest returns to the current period. Sessions group local chart results less than 60 minutes apart, including across app restarts. The most recent session remains available after an idle gap. Days use local midnight; weeks start Monday. Custom dates use `YYYY-MM-DD` and include both dates.

The recap has separate **Lamp upgrades**, **Score upgrades**, and **BP upgrades** sections. A chart can appear in several sections. Each row compares the best before the period with the best achieved inside it; repeated upgrades are consolidated. **New** means no earlier eligible local record was found. Score is EX score; lower BP is better. BP follows beatoraja: BAD + POOR + empty-POOR/MISS + unpassed notes. Saved `bad_points` takes precedence, including its unpassed-note penalty on failed plays. Older rows without saved BP use BAD + POOR + KPOOR only when completion can be established. Invalid BP values are excluded. Lamp upgrades are grouped by the achieved lamp, best lamp first, with alphabetical chart order within each group in both the screen and receipt. Different LN modes remain separate, with legacy unknown-mode records retained as baselines according to the existing best-score policy.

Recaps cover local chart records, not courses or imported IR scores. They follow the existing score and lamp eligibility policies. Stored titles allow deleted charts to remain visible; old records without a resolved chart hash are excluded because they cannot be safely attributed to a chart. The available history determines the baseline.

**Export PNG** creates one receipt-style PNG containing all three sections, regardless of the currently visible list page. The paper-colored receipt is 960 pixels wide; its height grows with the content and long chart titles wrap. It includes the profile, selected period, section counts, and before/after values. The canvas is independent of device resolution and orientation. Long receipts render in bounded strips and stream into a single PNG, including receipts taller than the GPU texture limit.

Desktop/Android files are saved under `Documents/recap_exports/<export-id>` in the app's document storage; iOS uses Photos. The screen reports the saved image path or an error. Incomplete PNG files are removed instead of exposing a truncated receipt. Keep the app in the foreground while it exports.
