/*
 * Copyright (C) 2015-2023 Slava Monich <slava@monich.com>
 * Copyright (C) 2015-2022 Jolla Ltd.
 *
 * You may use this file under the terms of the BSD license as follows:
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 *  1. Redistributions of source code must retain the above copyright
 *     notice, this list of conditions and the following disclaimer.
 *  2. Redistributions in binary form must reproduce the above copyright
 *     notice, this list of conditions and the following disclaimer
 *     in the documentation and/or other materials provided with the
 *     distribution.
 *  3. Neither the names of the copyright holders nor the names of its
 *     contributors may be used to endorse or promote products derived
 *     from this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED ``AS IS'' AND ANY EXPRESSED OR IMPLIED
 * WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES
 * OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED.
 * IN NO EVENT SHALL THE CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING,
 * BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
 * LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) ARISING
 * IN ANY WAY OUT OF THE USE OR INABILITY TO USE THIS SOFTWARE, EVEN
 * IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include "BooksBookModel.h"
#include "BooksTextStyle.h"
#include "BooksUtil.h"
#include "BooksDefs.h"

#include "HarbourDebug.h"
#include "HarbourTask.h"

#include "ZLTextHyphenator.h"

// ==========================================================================
// BooksBookModel::Data
// ==========================================================================

class BooksBookModel::Data {
public:
    Data(int aWidth, int aHeight) : iWidth(aWidth), iHeight(aHeight) {}

public:
    int iWidth;
    int iHeight;
    shared_ptr<BookModel> iBookModel;
    BooksPos::List iPageMarks;
    QByteArray iHash;
};

// ==========================================================================
// BooksBookModel::PagingTask
// ==========================================================================

class BooksBookModel::PagingTask : public HarbourTask
{
    Q_OBJECT

public:
    static const quint32 MarksFileVersion = 3;
    static const char MarksFileMagic[];
    struct MarksHeader {
        char magic[4];
        quint32 version;
        char hash[16];
        qint32 fontSize;
        qint32 leftMargin;
        qint32 rightMargin;
        qint32 topMargin;
        qint32 bottomMargin;
        quint32 count;
    } __attribute__((packed));

    PagingTask(QThreadPool* aPool, BooksBookModel* aModel,
        shared_ptr<Book> aBook);
    ~PagingTask();

    void performTask();

    static QString pageMarksFile(BooksBookModel* aModel);
    BooksPos::List loadPageMarks() const;
    bool acceptHash(const MarksHeader* aHeader) const;
    void savePageMarks() const;

Q_SIGNALS:
    void progress(int aProgress);

public:
    shared_ptr<Book> iBook;
    shared_ptr<ZLTextStyle> iTextStyle;
    BooksPaintContext iPaint;
    const BooksMargins iMargins;
    const QString iPageMarksFile;
    const QByteArray iHash;
    const QString iPath;
    Data* iData;
};

const char BooksBookModel::PagingTask::MarksFileMagic[] = "MARK";

BooksBookModel::PagingTask::PagingTask(QThreadPool* aPool,
    BooksBookModel* aModel, shared_ptr<Book> aBook) :
    HarbourTask(aPool),
    iBook(aBook),
    iTextStyle(aModel->textStyle()),
    iPaint(aModel->width(), aModel->height(), BooksColorScheme()),
    iMargins(aModel->margins()),
    iPageMarksFile(pageMarksFile(aModel)),
    iHash(aModel->book()->hash()),
    iPath(aModel->book()->path()),
    iData(NULL)
{
    aModel->connect(this, SIGNAL(done()), SLOT(onResetDone()));
    aModel->connect(this, SIGNAL(progress(int)), SLOT(onResetProgress(int)),
        Qt::QueuedConnection);
}

BooksBookModel::PagingTask::~PagingTask()
{
    delete iData;
}

QString BooksBookModel::PagingTask::pageMarksFile(BooksBookModel* aModel)
{
    return aModel->book()->storageFile(QString(".%1x%2" BOOKS_MARKS_FILE_SUFFIX).
        arg(aModel->width()).arg(aModel->height()));
}

bool BooksBookModel::PagingTask::acceptHash(const MarksHeader* aHeader) const
{
    // If the real hash is unknown, we accept any. The real one will
    // be later compared with the one we fetch from the .marks file
    return iHash.isEmpty() ||
        (iHash.size() == sizeof(aHeader->hash) &&
        !memcmp(iHash.constData(), aHeader->hash, sizeof(aHeader->hash)));
}

BooksPos::List BooksBookModel::PagingTask::loadPageMarks() const
{
    BooksPos::List list;
    QFile file(iPageMarksFile);
    if (file.open(QIODevice::ReadOnly)) {
        const qint64 size = file.size();
        uchar* map = file.map(0, size);
        if (map) {
            HDEBUG("reading" << qPrintable(iPageMarksFile));
            if (size > (int) sizeof(MarksHeader)) {
                const qint64 dataSize = size - sizeof(MarksHeader);
                const MarksHeader* hdr = (MarksHeader*)map;
                if (!memcmp(hdr->magic, MarksFileMagic, sizeof(hdr->magic)) &&
                    hdr->version == MarksFileVersion &&
                    acceptHash(hdr) &&
                    hdr->fontSize == iTextStyle->fontSize() &&
                    hdr->leftMargin == iMargins.iLeft &&
                    hdr->rightMargin == iMargins.iRight &&
                    hdr->topMargin == iMargins.iTop &&
                    hdr->bottomMargin == iMargins.iBottom &&
                    hdr->count > 0 && hdr->count * 12 == dataSize) {
                    const quint32* ptr = (quint32*)(hdr + 1);
                    for (quint32 i = 0; i < hdr->count; i++) {
                        quint32 para = *ptr++;
                        quint32 elem = *ptr++;
                        quint32 charIndex = *ptr++;
                        BooksPos pos(para, elem, charIndex);
                        if (!list.isEmpty()) {
                            const BooksPos& last = list.last();
                            if (last >= pos) {
                                HWARN(qPrintable(iPageMarksFile) <<
                                    "broken order");
                                list.clear();
                                break;
                            }
                        }
                        list.append(pos);
                    }
                } else {
                    HWARN(qPrintable(iPageMarksFile) << "header mismatch");
                }
            } else {
                HWARN(qPrintable(iPageMarksFile) << "too short");
            }
            file.unmap(map);
        } else {
            HWARN("error mapping" << qPrintable(iPageMarksFile));
        }
        file.close();
        if (list.isEmpty()) {
            HDEBUG("deleting" << qPrintable(iPageMarksFile));
            QFile::remove(iPageMarksFile);
        }
    }
    return list;
}

void BooksBookModel::PagingTask::savePageMarks() const
{
    MarksHeader hdr;
    QByteArray hash(iData->iHash);
    if (hash.size() == sizeof(hdr.hash) &&
        !iData->iPageMarks.isEmpty() &&
        !isCanceled()) {
        QFile file(iPageMarksFile);
        bool opened = file.open(QIODevice::ReadWrite);
        if (!opened) {
            // Most likely, the directory doesn't exist
            QDir dir = QFileInfo(iPageMarksFile).dir();
            if (dir.mkpath(dir.path())) {
                HDEBUG("created" << qPrintable(dir.path()));
                opened = file.open(QIODevice::ReadWrite);
            }
        }
        if (opened) {
            HDEBUG("writing" << qPrintable(iPageMarksFile));
            const int n = iData->iPageMarks.count();
            memset(&hdr, 0, sizeof(hdr));
            memcpy(hdr.magic, MarksFileMagic, sizeof(hdr.magic));
            hdr.version = MarksFileVersion;
            memcpy(hdr.hash, hash.constData(), sizeof(hdr.hash));
            hdr.fontSize = iTextStyle->fontSize();
            hdr.leftMargin = iMargins.iLeft;
            hdr.rightMargin = iMargins.iRight;
            hdr.topMargin = iMargins.iTop;
            hdr.bottomMargin = iMargins.iBottom;
            hdr.count = n;
            file.write((char*)&hdr, sizeof(hdr));
            for (int i = 0; i < n; i++) {
                const BooksPos& pos = iData->iPageMarks.at(i);
                quint32 data[3];
                data[0] = pos.iParagraphIndex;
                data[1] = pos.iElementIndex;
                data[2] = pos.iCharIndex;
                file.write((char*)data, sizeof(data));
            }
            file.close();
        } else {
            HWARN("can't open" << qPrintable(iPageMarksFile));
        }
    }
}

void BooksBookModel::PagingTask::performTask()
{
    if (!isCanceled()) {
        iData = new Data(iPaint.width(), iPaint.height());
        iData->iBookModel = new BookModel(iBook);
        iData->iHash = iHash;
        shared_ptr<ZLTextModel> model(iData->iBookModel->bookTextModel());
        ZLTextHyphenator::Instance().load(iBook->language());
        if (iData->iHash.isEmpty() && !isCanceled()) {
            // If hash is unknown then we need to compute it here and now.
            // It's a very rare occasion though.
            iData->iHash = BooksUtil::computeFileHashAndSetAttr(iPath, this);
        }
        if (!iData->iHash.isEmpty() && !isCanceled()) {
            // Load the cached marks
            iData->iPageMarks = loadPageMarks();
            if (iData->iPageMarks.isEmpty() && !isCanceled()) {
                // We have to do the hard way. This is going to take
                // a bit of time (from tens of seconds to minutes for
                // large books).
                BooksTextView view(iPaint, iTextStyle, iMargins);
                view.setModel(model);
                if (model->paragraphsNumber() > 0) {
                    BooksPos mark = view.rewind();
                    iData->iPageMarks.append(mark);
                    Q_EMIT progress(iData->iPageMarks.count());
                    while (!isCanceled() && view.nextPage()) {
                        mark = view.position();
                        iData->iPageMarks.append(mark);
                        Q_EMIT progress(iData->iPageMarks.count());
                    }
                }
                if (!isCanceled()) {
                    // Save it so that next time we won't have to do it again
                    savePageMarks();
                }
            }
        }
    }
    if (!isCanceled()) {
        HDEBUG(iData->iPageMarks.count() << "page(s)" << qPrintable(
            QString("%1x%2").arg(iData->iWidth).arg(iData->iHeight)));
    } else {
        HDEBUG("giving up" << qPrintable(QString("%1x%2").arg(iPaint.width()).
            arg(iPaint.height())) << "paging");
    }
}

// ==========================================================================
// BooksBookModel
// ==========================================================================

enum BooksBookModelRole {
    BooksBookModelPageIndex = Qt::UserRole,
    BooksBookModelBookPos
};

BooksBookModel::BooksBookModel(QObject* aParent) :
    QAbstractListModel(aParent),
    iResetReason(ReasonUnknown),
    iProgress(0),
    iBook(NULL),
    iPagingTask(NULL),
    iData(NULL),
    iData2(NULL),
    iSettings(BooksSettings::sharedInstance()),
    iTaskQueue(BooksTaskQueue::defaultQueue()),
    iPageStack(new BooksPageStack(this))
{
    iTextStyle = iSettings->textStyle(fontSizeAdjust());
    connect(iSettings.data(), SIGNAL(textStyleChanged()), SLOT(onTextStyleChanged()));
    connect(iPageStack, SIGNAL(changed()), SLOT(onPageStackChanged()));
    connect(iPageStack, SIGNAL(currentIndexChanged()), SLOT(onPageStackChanged()));
    HDEBUG("created");
#if QT_VERSION < 0x050000
    setRoleNames(roleNames());
#endif
}

BooksBookModel::~BooksBookModel()
{
    if (iPagingTask) iPagingTask->release(this);
    if (iBook) {
        iBook->disconnect(this);
        iBook->release();
        iBook = NULL;
    }
    delete iData;
    delete iData2;
    HDEBUG("destroyed");
}

void BooksBookModel::setBook(BooksBook* aBook)
{
    shared_ptr<Book> newBook;
    if (iBook != aBook) {
        const QString oldTitle(iTitle);
        if (iBook) {
            iBook->disconnect(this);
            iBook->release();
        }
        if (aBook) {
            (iBook = aBook)->retain();
            iBookRef = newBook;
            iTitle = aBook->title();
            iTextStyle = iSettings->textStyle(fontSizeAdjust());
            iPageStack->setStack(aBook->pageStack(), aBook->pageStackPos());
            connect(aBook, SIGNAL(fontSizeAdjustChanged()), SLOT(onTextStyleChanged()));
            connect(aBook, SIGNAL(hashChanged()), SLOT(onHashChanged()));
            HDEBUG(iTitle);
        } else {
            iBook = NULL;
            iBookRef.reset();
            iTitle = QString();
            iPageStack->clear();
            iPageStack->setPageMarks(BooksPos::List());
            HDEBUG("<none>");
        }
        startReset(ReasonLoading, true);
        if (oldTitle != iTitle) {
            Q_EMIT titleChanged();
        }
        Q_EMIT textStyleChanged();
        Q_EMIT bookModelChanged();
        Q_EMIT bookChanged();
    }
}

bool BooksBookModel::loading() const
{
    return (iPagingTask != NULL);
}

bool BooksBookModel::increaseFontSize()
{
    return iBook && iBook->setFontSizeAdjust(iBook->fontSizeAdjust()+1);
}

bool BooksBookModel::decreaseFontSize()
{
    return iBook && iBook->setFontSizeAdjust(iBook->fontSizeAdjust()-1);
}

void BooksBookModel::onPageStackChanged()
{
    if (iBook) {
        BooksPos::Stack stack = iPageStack->getStack();
        HDEBUG(stack.iList << stack.iPos);
        iBook->setPageStack(stack.iList, stack.iPos);
    }
}

void BooksBookModel::onHashChanged()
{
    const QByteArray hash(iBook->hash());
    HDEBUG(QString(hash.toHex()));
    if (!hash.isEmpty()) {
        if (iData2 && iData2->iHash != hash) {
            // There is no need to delete the stale file - it will be deleted
            // by the paging task. Deleting files on the UI thread is not a
            // very bright idea - the call may block for quite some time.
            delete iData2;
            iData2 = NULL;
        }
        if (iPagingTask &&
            !iPagingTask->iHash.isEmpty() &&
            iPagingTask->iHash != hash) {
            iPagingTask->release(this);
            iPagingTask = NULL;
            startReset(iResetReason);
        } else if (iData && iData->iHash != hash) {
            delete iData;
            iData = NULL;
            startReset(ReasonLoading);
        } else {
            HDEBUG("we are all set!");
        }
    }
}

int BooksBookModel::pageCount() const
{
    return iData ? iData->iPageMarks.count() : 0;
}

BooksPos::List BooksBookModel::pageMarks() const
{
    return iData ? iData->iPageMarks : BooksPos::List();
}

int BooksBookModel::fontSizeAdjust() const
{
    return iBook ? iBook->fontSizeAdjust() : 0;
}

BooksPos BooksBookModel::pageMark(int aPage) const
{
    return iData ? BooksPos::posAt(iData->iPageMarks, aPage) : BooksPos();
}

BooksPos BooksBookModel::linkPosition(const std::string& aLink) const
{
    if (iData && !iData->iBookModel.isNull()) {
        BookModel::Label label = iData->iBookModel->label(aLink);
        if (label.ParagraphNumber >= 0) {
            return BooksPos(label.ParagraphNumber, 0, 0);
        }
    }
    return BooksPos();
}

shared_ptr<BookModel> BooksBookModel::bookModel() const
{
    return iData ? iData->iBookModel : NULL;
}

shared_ptr<ZLTextModel> BooksBookModel::bookTextModel() const
{
    shared_ptr<ZLTextModel> model;
    if (iData && !iData->iBookModel.isNull()) {
        model = iData->iBookModel->bookTextModel();
    }
    return model;
}

shared_ptr<ZLTextModel> BooksBookModel::footnoteModel(const std::string& aId) const
{
    shared_ptr<ZLTextModel> model;
    if (iData && !iData->iBookModel.isNull()) {
        model = iData->iBookModel->footnoteModel(aId);
    }
    return model;
}

shared_ptr<ZLTextModel> BooksBookModel::contentsModel() const
{
    shared_ptr<ZLTextModel> model;
    if (iData && !iData->iBookModel.isNull()) {
        model = iData->iBookModel->contentsModel();
    }
    return model;
}

void BooksBookModel::setLeftMargin(int aMargin)
{
    if (iMargins.iLeft != aMargin) {
        iMargins.iLeft = aMargin;
        HDEBUG(aMargin);
        startReset();
        Q_EMIT leftMarginChanged();
    }
}

void BooksBookModel::setRightMargin(int aMargin)
{
    if (iMargins.iRight != aMargin) {
        iMargins.iRight = aMargin;
        HDEBUG(aMargin);
        startReset();
        Q_EMIT rightMarginChanged();
    }
}

void BooksBookModel::setTopMargin(int aMargin)
{
    if (iMargins.iTop != aMargin) {
        iMargins.iTop = aMargin;
        HDEBUG(aMargin);
        startReset();
        Q_EMIT topMarginChanged();
    }
}

void BooksBookModel::setBottomMargin(int aMargin)
{
    if (iMargins.iBottom != aMargin) {
        iMargins.iBottom = aMargin;
        HDEBUG(aMargin);
        startReset();
        Q_EMIT bottomMarginChanged();
    }
}

void BooksBookModel::emitBookPosChanged()
{
    const int n = pageCount();
    if (n > 0) {
        const QModelIndex topLeft(index(0));
        const QModelIndex bottomRight(index(n - 1));
        const QVector<int> roles(1, BooksBookModelBookPos);
        Q_EMIT dataChanged(topLeft, bottomRight, roles);
    }
}

void BooksBookModel::updateModel(int aPrevPageCount)
{
    const int newPageCount = pageCount();
    if (aPrevPageCount != newPageCount) {
        HDEBUG(aPrevPageCount << "->" << newPageCount);
        emitBookPosChanged();
        if (newPageCount > aPrevPageCount) {
            beginInsertRows(QModelIndex(), aPrevPageCount, newPageCount-1);
            endInsertRows();
        } else {
            beginRemoveRows(QModelIndex(), newPageCount, aPrevPageCount-1);
            endRemoveRows();
        }
        Q_EMIT pageCountChanged();
    }
}

void BooksBookModel::setSize(QSize aSize)
{
    if (iSize != aSize) {
        iSize = aSize;
        const int w = width();
        const int h = height();
        HDEBUG(aSize);
        if (iData && iData->iWidth == w && iData->iHeight == h) {
            HDEBUG("size didn't change");
        } else if (iData2 && iData2->iWidth == w && iData2->iHeight == h) {
            HDEBUG("switching to backup layout");
            const int oldModelPageCount = pageCount();
            Data* tmp = iData;
            iData = iData2;
            iData2 = tmp;
            // Cancel unnecessary paging task
            BooksLoadingSignalBlocker block(this);
            if (iPagingTask) {
                HDEBUG("not so fast please...");
                iPagingTask->release(this);
                iPagingTask = NULL;
            }
            updateModel(oldModelPageCount);
            iPageStack->setPageMarks(iData->iPageMarks);
            Q_EMIT pageMarksChanged();
            Q_EMIT jumpToPage(iPageStack->currentPage());
        } else {
            startReset(ReasonUnknown, false);
        }
        Q_EMIT sizeChanged();
    }
}

void BooksBookModel::onTextStyleChanged()
{
    HDEBUG(iTitle);
    shared_ptr<ZLTextStyle> newStyle = iSettings->textStyle(fontSizeAdjust());
    const int newFontSize = newStyle->fontSize();
    const int oldFontSize = iTextStyle->fontSize();
    const ResetReason reason =
        (newFontSize > oldFontSize) ? ReasonIncreasingFontSize :
        (newFontSize < oldFontSize) ? ReasonDecreasingFontSize :
        ReasonUnknown;
    iTextStyle = newStyle;
    startReset(reason);
    Q_EMIT textStyleChanged();
}

void BooksBookModel::startReset(ResetReason aResetReason, bool aFullReset)
{
    BooksLoadingSignalBlocker block(this);
    if (aResetReason == ReasonUnknown) {
        if (iResetReason == ReasonUnknown) {
            if (!iData && !iData2) {
                aResetReason = ReasonLoading;
            }
        } else {
            aResetReason = iResetReason;
        }
    }
    if (iPagingTask) {
        iPagingTask->release(this);
        iPagingTask = NULL;
    }
    const int oldPageCount(pageCount());
    if (oldPageCount > 0) {
        beginResetModel();
    }

    delete iData2;
    if (aFullReset) {
        delete iData;
        iData2 = NULL;
    } else {
        iData2 = iData;
    }
    iData = NULL;

    if (iBook && width() > 0 && height() > 0) {
        HDEBUG("starting" << qPrintable(QString("%1x%2").arg(width()).
            arg(height())) << "paging");
        (iPagingTask = new PagingTask(iTaskQueue->pool(), this,
            iBook->bookRef()))->submit();
    }

    if (oldPageCount > 0) {
        endResetModel();
        Q_EMIT pageMarksChanged();
        Q_EMIT pageCountChanged();
    }

    if (iProgress != 0) {
        iProgress = 0;
        Q_EMIT progressChanged();
    }

    if (iResetReason != aResetReason) {
        iResetReason = aResetReason;
        Q_EMIT resetReasonChanged();
    }
}

void BooksBookModel::onResetProgress(int aProgress)
{
    // progress -> onResetProgress is a queued connection, we may received
    // this event from the task that has already been canceled.
    if (iPagingTask == sender() && aProgress > iProgress) {
        iProgress = aProgress;
        Q_EMIT progressChanged();
    }
}

void BooksBookModel::onResetDone()
{
    HASSERT(sender() == iPagingTask);
    HASSERT(iPagingTask->iData);
    HASSERT(!iData);

    const QByteArray hash(iBook->hash());
    if (hash.isEmpty() || iPagingTask->iData->iHash == hash) {
        const int oldPageCount(pageCount());
        shared_ptr<BookModel> oldBookModel(bookModel());
        BooksLoadingSignalBlocker block(this);

        iData = iPagingTask->iData;
        iPagingTask->iData = NULL;
        iPagingTask->release(this);
        iPagingTask = NULL;

        updateModel(oldPageCount);
        iPageStack->setPageMarks(iData->iPageMarks);
        Q_EMIT jumpToPage(iPageStack->currentPage());
        Q_EMIT pageMarksChanged();
        if (oldBookModel != bookModel()) {
            Q_EMIT bookModelChanged();
        }
        if (iResetReason != ReasonUnknown) {
            iResetReason = ReasonUnknown;
            Q_EMIT resetReasonChanged();
        }
    } else {
        HDEBUG("oops");
        iPagingTask->release(this);
        iPagingTask = NULL;
    }
}

QHash<int,QByteArray> BooksBookModel::roleNames() const
{
    QHash<int, QByteArray> roles;
    roles.insert(BooksBookModelPageIndex, "pageIndex");
    roles.insert(BooksBookModelBookPos, "bookPos");
    return roles;
}

int BooksBookModel::rowCount(const QModelIndex&) const
{
    return pageCount();
}

QVariant BooksBookModel::data(const QModelIndex& aIndex, int aRole) const
{
    const int row = aIndex.row();
    if (row >= 0 && row < pageCount()) {
        switch ((BooksBookModelRole)aRole) {
        case BooksBookModelPageIndex:
            return row;
        case BooksBookModelBookPos:
            return QVariant::fromValue(iData->iPageMarks.at(row));
        }
    }
    return QVariant();
}

#include "BooksBookModel.moc"
