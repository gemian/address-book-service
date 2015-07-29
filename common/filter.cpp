/*
 * Copyright 2013 Canonical Ltd.
 *
 * This file is part of contact-service-app.
 *
 * contact-service-app is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; version 3.
 *
 * contact-service-app is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "filter.h"

#include <QtCore/QDataStream>
#include <QtCore/QByteArray>
#include <QtCore/QString>
#include <QtCore/QLocale>
#include <QtCore/QDebug>

#include <QtContacts/QContactGuid>
#include <QtContacts/QContactIdFilter>
#include <QtContacts/QContactDetailFilter>
#include <QtContacts/QContactUnionFilter>
#include <QtContacts/QContactIntersectionFilter>
#include <QtContacts/QContactManagerEngine>
#include <QtContacts/QContactDetailRangeFilter>
#include <QtContacts/QContactRelationshipFilter>

#include <phonenumbers/phonenumberutil.h>

using namespace QtContacts;

namespace galera
{

Filter::Filter(const QString &filter)
{
    m_filter = buildFilter(filter);
}

Filter::Filter(const QtContacts::QContactFilter &filter)
{
    m_filter = parseFilter(filter);
}

QString Filter::toString() const
{
    return toString(m_filter);
}

QtContacts::QContactFilter Filter::toContactFilter() const
{
    return m_filter;
}

bool Filter::test(const QContact &contact) const
{
    return testFilter(m_filter, contact);
}

QString Filter::countryCode()
{
    QString countryCode = QLocale::system().name().split("_").last();
    if (countryCode.size() < 2) {
        // fallback to US if no valid country code was provided, otherwise libphonenumber
        // will fail to parse any numbers
        return QString("US");
    }
    return countryCode;
}

bool Filter::isPhoneNumber(const QString &phoneNumber)
{
    static i18n::phonenumbers::PhoneNumberUtil *phonenumberUtil = i18n::phonenumbers::PhoneNumberUtil::GetInstance();
    std::string formattedNumber;
    i18n::phonenumbers::PhoneNumber number;
    i18n::phonenumbers::PhoneNumberUtil::ErrorType error;
    error = phonenumberUtil->Parse(phoneNumber.toStdString(), countryCode().toStdString(), &number);

    switch(error) {
    case i18n::phonenumbers::PhoneNumberUtil::INVALID_COUNTRY_CODE_ERROR:
        qWarning() << "Invalid country code for:" << phoneNumber;
        return false;
    case i18n::phonenumbers::PhoneNumberUtil::NOT_A_NUMBER:
        qWarning() << "The phone number is not a valid number:" << phoneNumber;
        return false;
    case i18n::phonenumbers::PhoneNumberUtil::TOO_SHORT_AFTER_IDD:
    case i18n::phonenumbers::PhoneNumberUtil::TOO_SHORT_NSN:
    case i18n::phonenumbers::PhoneNumberUtil::TOO_LONG_NSN:
        qWarning() << "Invalid phone number" << phoneNumber;
        return false;
    default:
        break;
    }
    return true;
}

bool Filter::comparePhoneNumbers(const QString &input, const QString &value, QContactFilter::MatchFlags flags)
{
    static i18n::phonenumbers::PhoneNumberUtil *phonenumberUtil = i18n::phonenumbers::PhoneNumberUtil::GetInstance();

    std::string stdPreprocessedInput(input.toStdString());
    std::string stdProcessedValue(value.toStdString());

    phonenumberUtil->NormalizeDiallableCharsOnly(&stdPreprocessedInput);
    phonenumberUtil->NormalizeDiallableCharsOnly(&stdProcessedValue);

    QString preprocessedInput = QString::fromStdString(stdPreprocessedInput);
    QString preprocessedValue = QString::fromStdString(stdProcessedValue);

    // if one of they does not contain digits return false
    if (preprocessedInput.isEmpty() || preprocessedValue.isEmpty()) {
        return false;
    }

    bool me = flags.testFlag(QContactFilter::MatchExactly);
    bool mc = flags.testFlag(QContactFilter::MatchContains);
    bool msw = flags.testFlag(QContactFilter::MatchStartsWith);
    bool mew = flags.testFlag(QContactFilter::MatchEndsWith);

    if (me || mc || msw || mew) {
        bool mer = (me ? preprocessedInput == preprocessedValue : true);
        bool mcr = (mc ? preprocessedValue.contains(preprocessedInput) : true);
        bool mswr = (msw ? preprocessedValue.startsWith(preprocessedInput) : true);
        bool mewr = (mew ? preprocessedValue.endsWith(preprocessedInput) : true);
        if (mewr && mswr && mcr && mer) {
            return true; // this detail meets all of the criteria which were required, and hence must match.
        }
    }

    // fallback case: handle as phone number

    // avoid problems with emergency numbers
    if (input.size() < 3 || value.size() < 3) {
        return (input == value);
    }

    // phone number compare
    i18n::phonenumbers::PhoneNumberUtil::MatchType match =
            phonenumberUtil->IsNumberMatchWithTwoStrings(input.toStdString(),
                                                         value.toStdString());
    return (match > i18n::phonenumbers::PhoneNumberUtil::NO_MATCH);
}

bool Filter::testFilter(const QContactFilter& filter, const QContact &contact)
{
    switch(filter.type()) {
        case QContactFilter::ContactDetailFilter:
            {
                const QContactDetailFilter cdf(filter);
                if (cdf.detailType() == QContactDetail::TypeUndefined)
                    return false;

                /* See if this contact has one of these details in it */
                const QList<QContactDetail>& details = contact.details(cdf.detailType());

                if (details.count() == 0)
                    return false; /* can't match */

                /* See if we need to check the values */
                if (cdf.detailField() == -1)
                    return true;  /* just testing for the presence of a detail of the specified type */

                if (cdf.matchFlags() & QContactFilter::MatchPhoneNumber) {
                    /* Doing phone number filtering.  We hand roll an implementation here, backends will obviously want to override this. */
                    QString input = cdf.value().toString();

                    /* Look at every detail in the set of details and compare */
                    for (int j = 0; j < details.count(); j++) {
                        const QContactDetail& detail = details.at(j);
                        const QString& valueString = detail.value(cdf.detailField()).toString();

                        if (comparePhoneNumbers(input, valueString, cdf.matchFlags())) {
                            return true;
                        }
                    }
                } else if (QContactManagerEngine::testFilter(filter, contact)) {
                    return true;
                }
            }
            break;
        case QContactFilter::IntersectionFilter:
            {
                /* XXX In theory we could reorder the terms to put the native tests first */
                const QContactIntersectionFilter bf(filter);
                const QList<QContactFilter>& terms = bf.filters();
                if (terms.count() > 0) {
                    for(int j = 0; j < terms.count(); j++) {
                        if (!testFilter(terms.at(j), contact)) {
                            return false;
                        }
                    }
                    return true;
                }
                // Fall through to end
            }
            break;

        case QContactFilter::UnionFilter:
            {
                /* XXX In theory we could reorder the terms to put the native tests first */
                const QContactUnionFilter bf(filter);
                const QList<QContactFilter>& terms = bf.filters();
                if (terms.count() > 0) {
                    for(int j = 0; j < terms.count(); j++) {
                        if (testFilter(terms.at(j), contact)) {
                            return true;
                        }
                    }
                    return false;
                }
                // Fall through to end
            }
            break;
        default:
            if (QContactManagerEngine::testFilter(filter, contact)) {
                return true;
            }
            break;
    }
    return false;
}

bool Filter::checkIsValid(const QList<QContactFilter> filters) const
{
    Q_FOREACH(const QContactFilter &f, filters) {
        switch (f.type()) {
            case QContactFilter::InvalidFilter:
                return false;
            case QContactFilter::IntersectionFilter:
                return checkIsValid(static_cast<QContactIntersectionFilter>(f).filters());
            case QContactFilter::UnionFilter:
                return checkIsValid(static_cast<QContactUnionFilter>(f).filters());
            default:
                return true;
        }
    }
    // list is empty
    return true;
}

bool Filter::isValid() const
{
    return checkIsValid(QList<QContactFilter>() << m_filter);
}

bool Filter::checkIsEmpty(const QList<QContactFilter> filters) const
{
    Q_FOREACH(const QContactFilter &f, filters) {
        switch (f.type()) {
        case QContactFilter::DefaultFilter:
            return true;
        case QContactFilter::IntersectionFilter:
            return checkIsEmpty(static_cast<QContactIntersectionFilter>(f).filters());
        case QContactFilter::UnionFilter:
            return checkIsEmpty(static_cast<QContactUnionFilter>(f).filters());
        default:
            return false;
        }
    }
    // list is empty
    return true;
}

bool Filter::isEmpty() const
{
    return checkIsEmpty(QList<QContactFilter>() << m_filter);
}

QString Filter::toString(const QtContacts::QContactFilter &filter)
{
    QByteArray filterArray;
    QDataStream filterData(&filterArray, QIODevice::WriteOnly);

    filterData << filter;

    return QString::fromLatin1(filterArray.toBase64());
}

QtContacts::QContactFilter Filter::buildFilter(const QString &filter)
{
    QContactFilter filterObject;
    QByteArray filterArray = QByteArray::fromBase64(filter.toLatin1());
    QDataStream filterData(&filterArray, QIODevice::ReadOnly);
    filterData >> filterObject;
    return filterObject;
}

QtContacts::QContactFilter Filter::parseFilter(const QtContacts::QContactFilter &filter)
{
    QContactFilter newFilter;
    switch (filter.type()) {
    case QContactFilter::IdFilter:
        newFilter = parseIdFilter(filter);
        break;
    case QContactFilter::UnionFilter:
        newFilter = parseUnionFilter(filter);
        break;
    case QContactFilter::IntersectionFilter:
        newFilter = parseIntersectionFilter(filter);
        break;
    default:
        return filter;
    }
    return newFilter;
}

QtContacts::QContactFilter Filter::parseUnionFilter(const QtContacts::QContactFilter &filter)
{
    QContactUnionFilter newFilter;
    const QContactUnionFilter *unionFilter = static_cast<const QContactUnionFilter*>(&filter);
    Q_FOREACH(const QContactFilter &f, unionFilter->filters()) {
        newFilter << parseFilter(f);
    }
    return newFilter;
}

QtContacts::QContactFilter Filter::parseIntersectionFilter(const QtContacts::QContactFilter &filter)
{
    QContactIntersectionFilter newFilter;
    const QContactIntersectionFilter *intersectFilter = static_cast<const QContactIntersectionFilter*>(&filter);
    Q_FOREACH(const QContactFilter &f, intersectFilter->filters()) {
        newFilter << parseFilter(f);
    }
    return newFilter;
}

QtContacts::QContactFilter Filter::parseIdFilter(const QContactFilter &filter)
{
    // ContactId to be serialized between process is necessary to instantiate the manager in both sides.
    // Since the dbus service does not instantiate the manager we translate it to QContactDetailFilter
    // using Guid values. This is possible because our server use the Guid to build the contactId.
    const QContactIdFilter *idFilter = static_cast<const QContactIdFilter*>(&filter);
    if (idFilter->ids().isEmpty()) {
        return filter;
    }

    QContactUnionFilter newFilter;

    Q_FOREACH(const QContactId &id, idFilter->ids()) {
        QContactDetailFilter detailFilter;
        detailFilter.setMatchFlags(QContactFilter::MatchExactly);
        detailFilter.setDetailType(QContactDetail::TypeGuid, QContactGuid::FieldGuid);

        detailFilter.setValue(id.toString().split(":").last());
        newFilter << detailFilter;
    }
    return newFilter;
}


}
