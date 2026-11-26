/*
 * src/_error.hpp
 * SPDX-License-Identifier: MPL-2.0
 *
 * Класс для исправления критической нехватки семантики (0.4.2.dev4)
 * Создан, чтобы семантически разделить ошибки, бросаемые ридерами.
 *
 * Сейчас (0.4.2.dev4):
 *     Неправильная конфигурация -> ридер падает, пытается восстановиться, но
 * неправильную конфигурацию нельзя восстановить -> бесконечный restart loop
 *
 * Предполагаемое поведение после введения (0.4.3+):
 *     Неправильная конфигурация -> ридер сразу закроется и сообщит о
 * некорректной конфигурации (ErrorKind.UsupportedConfiguration) ->
 * написанный другими людьми код сможет как минимум предусмотреть fallback
 *
 * Аналогично для других ошибок, в общем, добавляем семантику ошибок, чтобы
 * перестать пересоздавать подключение/hwcontex/etc., не иметь ситуаций
 * с бесконечными restart loop, и дать юзеру конкретное понимание того,
 * что пошло не так.
 */
#pragma once

#include <stdexcept>
#include <string>
#include <utility>

enum class SessionOutcome { Eof, Stopped };

enum class ErrorKind {
    CorruptHardwareFrame,
    SourceUnavailable,
    ConnectionLost,
    DecodeFailed,
    UnsupportedConfig,
    HardwareFailure,
    InternalError
};

class ReaderError : public std::runtime_error {
  public:
    ReaderError(ErrorKind kind, std::string message, int ffmpeg_code = 0)
        : std::runtime_error(std::move(message)), kind_(kind), ffmpeg_code_(ffmpeg_code) {}

    [[nodiscard]]
    ErrorKind kind() const noexcept {
        return kind_;
    }

    [[nodiscard]]
    int ffmpeg_code() const noexcept {
        return ffmpeg_code_;
    }

  private:
    ErrorKind kind_;
    int ffmpeg_code_;
};
