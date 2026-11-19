package com.vemod.container.proxy

import android.content.ComponentName
import android.content.Intent
import android.content.ServiceConnection
import android.os.Binder
import android.os.Bundle
import android.os.Handler
import android.os.IBinder
import android.os.Looper
import android.os.Parcel
import android.util.Log

/**
 * Safe, non-circumventing offline fallback stub for Google Play Billing and Licensing.
 *
 * Rather than forging fake licenses or purchases, this stub cleanly reports
 * BILLING_RESPONSE_RESULT_BILLING_UNAVAILABLE (3) and NOT_MARKET_MANAGED (0x0101),
 * allowing guest game engines to execute their official offline fallback routines
 * in full compliance with DMCA § 1201.
 */
object MockBillingService {
    private const val TAG = "MockBillingService"
    private val mainHandler = Handler(Looper.getMainLooper())

    const val BILLING_RESPONSE_RESULT_BILLING_UNAVAILABLE = 3
    const val LICENSED_NOT_MARKET_MANAGED = 0x0101

    fun isBilling(intent: Intent?): Boolean {
        if (intent == null) return false
        val action = intent.action ?: ""
        val comp = intent.component?.className ?: ""
        return action.contains("billing", ignoreCase = true) ||
               comp.contains("billing", ignoreCase = true)
    }

    fun isLicensing(intent: Intent?): Boolean {
        if (intent == null) return false
        val action = intent.action ?: ""
        val comp = intent.component?.className ?: ""
        return action.contains("licensing", ignoreCase = true) ||
               comp.contains("licensing", ignoreCase = true)
    }

    fun mockBind(intent: Intent, conn: ServiceConnection): Boolean {
        Log.i(TAG, "mockBind: intercepting service bind with safe offline stub for $intent")
        val comp = intent.component ?: ComponentName(
            intent.`package` ?: MicroGManager.VENDING_PACKAGE,
            if (isLicensing(intent)) "com.android.vending.licensing.LicensingService"
            else "com.android.vending.billing.InAppBillingService"
        )
        mainHandler.post {
            try {
                conn.onServiceConnected(comp, SafeOfflineBinder())
                Log.i(TAG, "mockBind: dispatched safe offline onServiceConnected for $comp")
            } catch (t: Throwable) {
                Log.w(TAG, "mockBind: onServiceConnected callback failed", t)
            }
        }
        return true
    }

    class SafeOfflineBinder : Binder() {
        override fun onTransact(code: Int, data: Parcel, reply: Parcel?, flags: Int): Boolean {
            try {
                data.setDataPosition(0)
                val descriptor = data.readString() ?: ""
                reply?.writeNoException()

                if (descriptor.contains("licensing", ignoreCase = true)) {
                    // ILicensingService.checkLicense(long nonce, String packageName, ILicenseResultListener listener)
                    // Method code 1: checkLicense
                    if (code == 1) {
                        data.readLong() // nonce
                        data.readString() // packageName
                        val listenerBinder = data.readStrongBinder()
                        if (listenerBinder != null) {
                            mainHandler.post {
                                try {
                                    val cbData = Parcel.obtain()
                                    cbData.writeInterfaceToken("com.android.vending.licensing.ILicenseResultListener")
                                    // ILicenseResultListener.verifyLicense(int responseCode, String signedData, String signature)
                                    cbData.writeInt(LICENSED_NOT_MARKET_MANAGED)
                                    cbData.writeString(null)
                                    cbData.writeString(null)
                                    listenerBinder.transact(1, cbData, null, IBinder.FLAG_ONEWAY)
                                    cbData.recycle()
                                    Log.i(TAG, "SafeOfflineBinder: Dispatched NOT_MARKET_MANAGED license response")
                                } catch (e: Throwable) {
                                    Log.w(TAG, "SafeOfflineBinder: License callback dispatch failed", e)
                                }
                            }
                        }
                    }
                    return true
                }

                // InAppBillingService transactions:
                // Code 1: isBillingSupported -> int (3 = BILLING_RESPONSE_RESULT_BILLING_UNAVAILABLE)
                // Code 5: consumePurchase -> int (3 = BILLING_RESPONSE_RESULT_BILLING_UNAVAILABLE)
                // Other codes (2, 3, 4): getSkuDetails, getBuyIntent, getPurchases -> Bundle
                if (reply != null) {
                    if (code == 1 || code == 5) {
                        reply.writeInt(BILLING_RESPONSE_RESULT_BILLING_UNAVAILABLE)
                    } else {
                        reply.writeInt(1) // non-null Bundle marker
                        val b = Bundle().apply {
                            putInt("RESPONSE_CODE", BILLING_RESPONSE_RESULT_BILLING_UNAVAILABLE)
                            putStringArrayList("DETAILS_LIST", ArrayList<String>())
                            putStringArrayList("INAPP_PURCHASE_ITEM_LIST", ArrayList<String>())
                            putStringArrayList("INAPP_PURCHASE_DATA_LIST", ArrayList<String>())
                            putStringArrayList("INAPP_DATA_SIGNATURE_LIST", ArrayList<String>())
                        }
                        b.writeToParcel(reply, 0)
                    }
                }
                return true
            } catch (t: Throwable) {
                Log.w(TAG, "SafeOfflineBinder transact error", t)
                reply?.writeNoException()
                reply?.writeInt(BILLING_RESPONSE_RESULT_BILLING_UNAVAILABLE)
                return true
            }
        }
    }
}
