using System.Collections.Generic;
using System.Text;

namespace SoloASBridgeUht;

internal static class SoloASBridgeExporterSupport
{
    internal static void AppendFunctions(
        StringBuilder builder,
        IReadOnlyList<SoloASBridgeExporter.FunctionRecord> records
    )
    {
        if (records.Count == 0)
        {
            builder.Append("    { nullptr, nullptr },\n");
            return;
        }
        foreach (SoloASBridgeExporter.FunctionRecord record in records)
        {
            builder.Append("    { \"").Append(record.Owner).Append("\", \"")
                .Append(record.Name).Append("\" },\n");
        }
    }
}
