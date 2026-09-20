#include <QApplication>
#include <QLabel>
#include <QMainWindow>
#include <QStatusBar>

extern "C" int openbc_run_gui() {
    int argc = 1;
    char application_name[] = "openbc-qt";
    char* argv[] = {application_name, nullptr};
    QApplication application(argc, argv);

    QMainWindow window;
    window.setWindowTitle("OpenBC");
    window.resize(960, 600);
    window.setCentralWidget(new QLabel("OpenBC folder comparison", &window));
    window.statusBar()->showMessage("Ready");
    window.show();

    return application.exec();
}