// ============================================================================
// stack_layout_probe —— QVBoxLayout 用【栈对象】到底会怎样？
//
// 背景（M1-Q2 的争议点）：
//   MainWindow::buildUi() 里写的是
//       QVBoxLayout *mainLayout = new QVBoxLayout(central);
//   骨架注释（MainWindow.cpp:97-98）说：
//       "写成 QVBoxLayout root(central); 也能编译，但函数一返回它就析构了，
//        控件会瞬间失去布局 —— 界面变成一堆叠在一起的散件。"
//
//   问题：注释说"失去布局"，另一种说法是"central 析构时 delete 子对象 → double free → 崩"。
//   到底哪个对？本探针用 Qt 6.10.2 实机跑。
//
// 五种形态：
//   heap              正确写法（基线）：堆 layout → 看控件被布局正常撑开
//   stack-owner-alive 栈 layout，父对象 central 比它活得久（= buildUi 的真实形态）
//                     → 看它崩不崩；看 central->layout() 变什么；看控件几何尺寸
//   stack-owner-dead  栈 layout，父对象 central 先被 delete（真 double free 形态）
//                     → 看这条到底崩不崩（这才是"会崩"那句话成立的条件）
//   stack-show-after  ★ 真实时序：layout 先死，show() 之后才发生
//                     （buildUi() 里根本没有 show，show 发生在 MainWindow 构造完之后）
//                     → 看控件到底有没有被摆过位
//   stretch-zero      ★ 不给拉伸因子时，多出来的高度归谁？
//                     → a) 三个 QPushButton（垂直都非 Expanding）
//                       b) QPushButton + QPlainTextEdit（后者垂直 Expanding）
//
// 用法：stack_layout_probe.exe [heap|stack-owner-alive|stack-owner-dead|stack-show-after|stretch-zero]
// ============================================================================

#include <QApplication>
#include <QLabel>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QVBoxLayout>
#include <QWidget>

#include <cstdio>

static void dumpGeom(const char *who, QWidget *w)
{
    printf("        %-10s geometry=(%d,%d %dx%d)\n",
           who, w->x(), w->y(), w->width(), w->height());
}

static void dump(const char *tag, QWidget *central, QWidget *b1, QWidget *b2, QWidget *l1)
{
    printf("  [%s] central->layout()=%p   central.children()=%d\n",
           tag, static_cast<void *>(central->layout()), static_cast<int>(central->children().size()));
    dumpGeom("btn1", b1);
    dumpGeom("btn2", b2);
    dumpGeom("label", l1);
}

int main(int argc, char *argv[])
{
    // ★ stdout 必须无缓冲：这条探针有意触发 UB，异常终止会丢掉整个缓冲区，
    //   到时候"什么都没打印就没站起来"会被误判成"没跑起来"。
    setvbuf(stdout, nullptr, _IONBF, 0);

    QApplication app(argc, argv);

    const QString mode = argc > 1 ? QString::fromLatin1(argv[1])
                                  : QStringLiteral("stack-owner-alive");
    printf("=== mode = %s ===\n", qPrintable(mode));

    // ------------------------------------------------------------------
    // ① heap —— 正确写法，基线。控件应被布局撑满 400x300
    // ------------------------------------------------------------------
    if (mode == QStringLiteral("heap")) {
        QWidget *central = new QWidget;
        central->resize(400, 300);

        QVBoxLayout *lay = new QVBoxLayout(central);
        auto *b1 = new QPushButton(QStringLiteral("b1"));
        auto *b2 = new QPushButton(QStringLiteral("b2"));
        auto *l1 = new QLabel(QStringLiteral("l1"));
        lay->addWidget(b1);
        lay->addWidget(b2);
        lay->addWidget(l1);

        central->show();
        app.processEvents();
        dump("heap", central, b1, b2, l1);

        printf("  >> delete central\n");
        delete central;
        printf("  delete central -> OK, NO CRASH\n");
        return 0;
    }

    // ------------------------------------------------------------------
    // ② stack-owner-alive —— 栈 layout，父对象活得更久
    //    这就是 buildUi() 里写 QVBoxLayout root(central); 的真实形态
    //    （central 是 new 出来的，活到 MainWindow 析构）
    // ------------------------------------------------------------------
    if (mode == QStringLiteral("stack-owner-alive")) {
        QWidget *central = new QWidget;
        central->resize(400, 300);

        QPushButton *b1 = nullptr;
        QPushButton *b2 = nullptr;
        QLabel *l1 = nullptr;

        {
            QVBoxLayout stackLayout(central);   // ← 栈对象，parent = central
            b1 = new QPushButton(QStringLiteral("b1"));
            b2 = new QPushButton(QStringLiteral("b2"));
            l1 = new QLabel(QStringLiteral("l1"));
            stackLayout.addWidget(b1);
            stackLayout.addWidget(b2);
            stackLayout.addWidget(l1);

            central->show();
            app.processEvents();
            printf("  [in-scope]  central->layout()=%p   <- 栈 layout 自己的地址\n",
                   static_cast<void *>(central->layout()));
            dump("in-scope", central, b1, b2, l1);
        }   // ← 栈 layout 在这里析构（buildUi() 返回时就是这一刻）

        printf("  -- 作用域结束，栈 layout 已析构 --\n");
        dump("out-of-scope", central, b1, b2, l1);
        printf("      ^ central->layout() 还指着刚析构的那个栈地址吗？\n");

        printf("  >> 现在 delete central（若 double free，就崩在这里）\n");
        delete central;
        printf("  delete central -> OK, NO CRASH\n");
        return 0;
    }

    // ------------------------------------------------------------------
    // ③ stack-owner-dead —— 栈 layout，父对象【先】死
    //    这才是"栈对象设了 parent 会 double free"成立的条件
    // ------------------------------------------------------------------
    if (mode == QStringLiteral("stack-owner-dead")) {
        QWidget *central = new QWidget;
        central->resize(400, 300);

        QVBoxLayout *addrForPrint = nullptr;
        {
            QVBoxLayout stackLayout(central);
            stackLayout.addWidget(new QPushButton(QStringLiteral("b1")));
            addrForPrint = &stackLayout;
            printf("  [in-scope]  central->layout()=%p   stack=&%p\n",
                   static_cast<void *>(central->layout()), static_cast<void *>(addrForPrint));

            printf("  >> 先 delete central —— 它会连带 delete children 里的栈 layout\n");
            delete central;
            printf("  delete central 返回了（栈上的 layout 已被删掉）\n");
            printf("  >> 现在离开作用域，栈 layout 会【第二次析构】\n");
        }

        printf("  [out-of-scope] 竟然走到了这里（stack=%p）—— 本次未复现崩溃\n",
               static_cast<void *>(addrForPrint));
        return 0;
    }

    // ------------------------------------------------------------------
    // ④ stack-show-after —— 真实时序：layout 先死，show 之后才来
    //    buildUi() 里没有 show()，MainWindow 构造完才 show。
    //    所以问的是：控件到底有没有被"摆过位"？
    // ------------------------------------------------------------------
    if (mode == QStringLiteral("stack-show-after")) {
        QWidget *central = new QWidget;
        central->resize(400, 300);

        QPushButton *b1 = nullptr;
        QPushButton *b2 = nullptr;
        QLabel *l1 = nullptr;

        {
            QVBoxLayout stackLayout(central);
            b1 = new QPushButton(QStringLiteral("b1"));
            b2 = new QPushButton(QStringLiteral("b2"));
            l1 = new QLabel(QStringLiteral("l1"));
            stackLayout.addWidget(b1);
            stackLayout.addWidget(b2);
            stackLayout.addWidget(l1);
            printf("  [in-scope, 未 show] layout=%p\n", static_cast<void *>(central->layout()));
            dump("in-scope", central, b1, b2, l1);
        }   // ← layout 在这里没了

        printf("  -- layout 已析构，现在才 show()（= buildUi() 返回之后）--\n");
        central->show();
        app.processEvents();
        dump("after-show", central, b1, b2, l1);
        printf("      ^ 控件是被布局摆好了，还是全叠在 (0,0)？\n");
        printf("  >> delete central\n");
        delete central;
        printf("  delete central -> OK, NO CRASH\n");
        return 0;
    }

    // ------------------------------------------------------------------
    // ⑤ stretch-zero —— 不传拉伸因子，多出来的高度归谁？
    // ------------------------------------------------------------------
    if (mode == QStringLiteral("stretch-zero")) {
        printf("  --- a) 三个 QPushButton（垂直都不是 Expanding）---\n");
        {
            QWidget *central = new QWidget;
            central->resize(400, 900);            // 竖向给足空间
            QVBoxLayout *lay = new QVBoxLayout(central);
            auto *b1 = new QPushButton(QStringLiteral("b1"));
            auto *b2 = new QPushButton(QStringLiteral("b2"));
            auto *b3 = new QPushButton(QStringLiteral("b3"));
            lay->addWidget(b1);                   // ← 三个都不传第二个参数
            lay->addWidget(b2);
            lay->addWidget(b3);
            central->show();
            app.processEvents();
            printf("        central 高度=900, 期望: 若平均分配则每个约 300\n");
            dumpGeom("btn1", b1);
            dumpGeom("btn2", b2);
            dumpGeom("btn3", b3);
            delete central;
        }

        printf("  --- b) QPushButton + QPlainTextEdit（后者垂直 Expanding）---\n");
        {
            QWidget *central = new QWidget;
            central->resize(400, 900);
            QVBoxLayout *lay = new QVBoxLayout(central);
            auto *b1 = new QPushButton(QStringLiteral("b1"));
            auto *te = new QPlainTextEdit;
            lay->addWidget(b1);                   // ← 同样不传第二个参数
            lay->addWidget(te);
            central->show();
            app.processEvents();
            printf("        central 高度=900, 期望: 若平均分配则每个约 450\n");
            dumpGeom("btn1", b1);
            dumpGeom("text", te);
            delete central;
        }
        return 0;
    }

    printf("unknown mode: %s\n", qPrintable(mode));
    return 2;
}
