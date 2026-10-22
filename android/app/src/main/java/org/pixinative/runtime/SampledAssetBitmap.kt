package org.pixinative.runtime

import android.content.res.AssetManager
import android.graphics.Bitmap
import android.graphics.BitmapFactory
import android.widget.ImageView

/** UI artwork only. Game textures must retain their original atlas dimensions. */
internal object SampledAssetBitmap {
    data class Bounds(val width: Int, val height: Int)

    fun bounds(assets: AssetManager, path: String): Bounds {
        val options = BitmapFactory.Options().apply { inJustDecodeBounds = true }
        assets.open(path).use { BitmapFactory.decodeStream(it, null, options) }
        check(options.outWidth > 0 && options.outHeight > 0) { "Could not read artwork bounds: $path" }
        return Bounds(options.outWidth, options.outHeight)
    }

    // The sampled bitmap must still cover the view in both dimensions. Never
    // subsample an image that would then have to be enlarged to fill its view.
    fun sampleSize(bounds: Bounds, width: Int, height: Int): Int {
        require(width > 0 && height > 0)
        var sample = 1
        while (bounds.width / (sample * 2) >= width && bounds.height / (sample * 2) >= height) sample *= 2
        return sample
    }

    fun decode(assets: AssetManager, path: String, width: Int, height: Int): Bitmap =
        decodeSample(assets, path, sampleSize(bounds(assets, path), width, height))

    private fun decodeSample(assets: AssetManager, path: String, sample: Int): Bitmap {
        val options = BitmapFactory.Options().apply { inSampleSize = sample; inScaled = false }
        return assets.open(path).use {
            checkNotNull(BitmapFactory.decodeStream(it, null, options)) { "Could not decode artwork: $path" }
        }
    }

    /** Decode after measurement, and reload at adequate resolution on window resize. */
    fun bind(view: ImageView, assets: AssetManager, paths: List<String>, install: (List<Bitmap>) -> Unit) {
        val bounds = paths.map { bounds(assets, it) }
        var previousSamples: List<Int>? = null
        view.addOnLayoutChangeListener { _, left, top, right, bottom, _, _, _, _ ->
            val width = right - left - view.paddingLeft - view.paddingRight
            val height = bottom - top - view.paddingTop - view.paddingBottom
            if (width <= 0 || height <= 0) return@addOnLayoutChangeListener
            val samples = bounds.map { sampleSize(it, width, height) }
            if (samples != previousSamples) {
                install(paths.mapIndexed { index, path -> decodeSample(assets, path, samples[index]) })
                previousSamples = samples
            }
        }
    }
}
