import { appBuilder } from '@canton/framework'
import { defineFontFamily } from '@canton/framework/ui'
import ShowcaseApp from './ShowcaseApp.cta'
import { fontFamily } from './theme.ts'

// 画面里的文字都用自带的测试字体；声明预加载，首帧等字体到齐再出
defineFontFamily(fontFamily, {
    faces: [{ src: '/fonts/CantonTestSans-Regular.ttf' }, { src: '/fonts/CantonTestSans-Bold.ttf' }, { src: '/fonts/CantonTestSans-Italic.ttf' }],
    preload: true,
})

export default appBuilder(ShowcaseApp).build()
