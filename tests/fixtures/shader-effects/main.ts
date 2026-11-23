import { appBuilder } from '@canton/framework'
import { defineFontFamily } from '@canton/framework/ui'
import ShaderEffectsApp from './ShaderEffectsApp.cta'
import { fontFamily } from './theme.ts'

defineFontFamily(fontFamily, {
    faces: [
        { src: '/fonts/CantonTestSans-Regular.ttf' },
        { src: '/fonts/CantonTestSans-Bold.ttf' },
        { src: '/fonts/CantonTestSans-Italic.ttf' },
    ],
    preload: true,
})

export default appBuilder(ShaderEffectsApp).build()
