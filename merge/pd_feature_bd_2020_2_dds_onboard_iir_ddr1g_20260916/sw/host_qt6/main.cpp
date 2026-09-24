#include "main_window.h"

#include <QApplication>
#include <QTimer>

#include <cstdio>

/*
 * 开发用选项：--screenshot <路径>
 * 显示窗口后延迟抓一张图再退出，用于核对布局是否在目标分辨率下放得下。
 * 布局问题（控件被切掉、日志被挤出屏幕）光看代码判断不了，必须看图。
 */
int main(int argc, char *argv[])
{
    QApplication app(argc, argv);
    QApplication::setApplicationName(QStringLiteral("PD Acquisition Desktop"));
    QApplication::setOrganizationName(QStringLiteral("PD Prototype"));

    MainWindow window;
    window.show();

    const QStringList arguments = app.arguments();
    const int shotIndex = arguments.indexOf(QStringLiteral("--screenshot"));
    if (shotIndex > 0 && shotIndex + 1 < arguments.size()) {
        const QString path = arguments.at(shotIndex + 1);
        /* 可选第二参数 WxH：按指定尺寸渲染，用来核对小屏下是否放得下。 */
        if (shotIndex + 2 < arguments.size()) {
            const QStringList size = arguments.at(shotIndex + 2).split(QLatin1Char('x'));
            if (size.size() == 2)
                window.resize(size.at(0).toInt(), size.at(1).toInt());
        }
        QTimer::singleShot(800, &app, [&window, path] {
            const bool ok = window.grab().save(path);
            std::printf("%s\n", qPrintable(
                ok ? QStringLiteral("已保存界面截图：%1").arg(path)
                   : QStringLiteral("保存界面截图失败：%1").arg(path)));
            QApplication::quit();
        });
    }
    return app.exec();
}
