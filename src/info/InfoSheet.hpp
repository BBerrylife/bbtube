#ifndef INFOSHEET_HPP_
#define INFOSHEET_HPP_

#include "src/utils/BaseSheet.hpp"

#include <QObject>
#include <bb/cascades/Sheet>
#include <bb/cascades/TouchEvent>

using namespace bb::cascades;

class InfoSheet: public BaseSheet
{
    Q_OBJECT
public:
    InfoSheet();
    virtual ~InfoSheet()
    {
    }
private slots:
    void onCrackberryActionItemClick();
    void onDonateActionItemClick();
    void onBBerryLifeTouch(bb::cascades::TouchEvent *event);
};


#endif /* INFOSHEET_HPP_ */
