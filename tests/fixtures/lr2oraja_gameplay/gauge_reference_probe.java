import java.io.BufferedReader;
import java.io.InputStreamReader;
import bms.model.BMSModel;
import bms.player.beatoraja.play.BMSPlayerRule;
import bms.player.beatoraja.play.GaugeProperty;
import bms.player.beatoraja.play.GrooveGauge;

// All gauge calculation and TOTAL validation execute the pinned source classes.
class GaugeReferenceProbe {
    public static void main(String[] args) throws Exception {
        BufferedReader reader = new BufferedReader(new InputStreamReader(System.in));
        String line;
        StringBuilder output = new StringBuilder();
        while ((line = reader.readLine()) != null) {
            String[] fields = line.split(" ");
            int profile = Integer.parseInt(fields[1]);
            int selected = Integer.parseInt(fields[2]);
            BMSModel model = new BMSModel();
            model.notes = Integer.parseInt(fields[3]);
            model.total = fields[5].equals("1") ? Double.parseDouble(fields[4]) : 0;
            BMSPlayerRule.validate(model);
            GaugeProperty property = switch (profile) {
                case 2 -> GaugeProperty.FIVEKEYS;
                case 3 -> GaugeProperty.SEVENKEYS;
                case 4 -> GaugeProperty.PMS;
                case 5 -> GaugeProperty.KEYBOARD;
                default -> GaugeProperty.LR2;
            };
            boolean course = profile >= 1 && profile <= 6;
            GrooveGauge gauge = GrooveGauge.create(model, selected, course ? 1 : 0, property);
            var definition = gauge.getGauge().getProperty();
            output.setLength(0);
            output.append(Long.toUnsignedString(Double.doubleToRawLongBits(model.total)));
            for (float value : new float[]{definition.init, definition.min,
                    definition.max, definition.border, definition.death}) {
                output.append(' ').append(Integer.toUnsignedString(Float.floatToRawIntBits(value)));
            }
            for (float value : definition.value) {
                output.append(' ').append(Integer.toUnsignedString(Float.floatToRawIntBits(value)));
            }
            gauge.setValue(gauge.getType(), Integer.parseInt(fields[6]));
            emitState(output, gauge);
            int judge = Integer.parseInt(fields[7]);
            float rate = Float.parseFloat(fields[8]);
            float mine = Float.parseFloat(fields[9]);
            int steps = Integer.parseInt(fields[10]);
            for (int step = 0; step < steps; ++step) {
                if (judge < 0 || (judge == 6 && step % 7 == 6)) gauge.addValue(mine);
                else gauge.update(judge == 6 ? step % 7 : judge, rate);
                emitState(output, gauge);
            }
            System.out.println(output);
        }
    }
    private static void emitState(StringBuilder output, GrooveGauge gauge) {
        output.append(' ').append(Integer.toUnsignedString(Float.floatToRawIntBits(gauge.getValue())))
              .append(' ').append(gauge.isQualified() ? 1 : 0);
    }
}
