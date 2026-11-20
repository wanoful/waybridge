using Microsoft.UI.Xaml.Controls;
using Windows.Foundation;

namespace Waybridge.Control;

internal sealed class LauncherCategoryPanel : Panel
{
    private const double ColumnSpacing = 6;
    private const double RowSpacing = 4;

    protected override Size MeasureOverride(Size availableSize) => LayoutRows(availableSize.Width, arrange: false);

    protected override Size ArrangeOverride(Size finalSize)
    {
        LayoutRows(finalSize.Width, arrange: true);
        return finalSize;
    }

    private Size LayoutRows(double width, bool arrange)
    {
        double right = 0, top = 0, rowHeight = 0, widest = 0;
        foreach (var child in Children)
        {
            if (!arrange) child.Measure(new Size(width, double.PositiveInfinity));
            var size = child.DesiredSize;
            double left = right == 0 ? 0 : right + ColumnSpacing;
            if (right > 0 && left + size.Width > width)
            {
                left = 0;
                top += rowHeight + RowSpacing;
                rowHeight = 0;
            }
            if (arrange) child.Arrange(new Rect(left, top, size.Width, size.Height));
            right = left + size.Width;
            rowHeight = Math.Max(rowHeight, size.Height);
            widest = Math.Max(widest, right);
        }
        return new Size(widest, top + rowHeight);
    }
}
