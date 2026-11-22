import { CANTON_PACKAGE_VERSION } from '@canton/shared'

export const CANTON_VERSION = CANTON_PACKAGE_VERSION

export const PUBLIC_PLUGIN_NAME = "canton-framework"

export const CANTON_DEFINE_KEYS = ["__DEV__", "__TEST__"] as const

export const CANTON_DEFINES = { __DEV__: "false", __TEST__: "false" } as const satisfies Record<(typeof CANTON_DEFINE_KEYS)[number], string>

export const CANTON_MACRO_PATTERN = new RegExp(`\\b__(?:${CANTON_DEFINE_KEYS.map((key) => key.slice(2, -2)).join("|")})__\\b`)
